// Headless throughput benchmark for the `vte` crate -- the actual VT
// parser Alacritty is built on. Reads the exact same workload files
// bench/sink_bench.cpp does and times vte::Parser::advance() over them.
//
// The Perform impl below deliberately does *comparable* bookkeeping to
// sink's TerminalGrid (a flat cell buffer, cursor tracking, SGR color
// state, full-buffer shift on scroll) rather than an optimized ring
// buffer, even though real Alacritty uses one. The point of this
// benchmark is to isolate "parsing + basic cell-write cost" as fairly as
// possible -- giving only the Rust side a smarter grid would measure grid
// architecture, not parser speed, and conflate the two.
use std::env;
use std::fs;
use std::time::Instant;
use vte::{Params, ParamsIter, Parser, Perform};

#[derive(Clone, Copy, Default)]
struct Cell {
    ch: char,
    fg: (u8, u8, u8),
    bg: (u8, u8, u8),
}

struct Grid {
    cols: usize,
    rows: usize,
    cells: Vec<Cell>,
    cursor_row: usize,
    cursor_col: usize,
    cur_fg: (u8, u8, u8),
    cur_bg: (u8, u8, u8),
}

impl Grid {
    fn new(cols: usize, rows: usize) -> Self {
        Grid {
            cols,
            rows,
            cells: vec![Cell::default(); cols * rows],
            cursor_row: 0,
            cursor_col: 0,
            cur_fg: (230, 230, 230),
            cur_bg: (0, 0, 0),
        }
    }

    fn write_char(&mut self, c: char) {
        if self.cursor_col >= self.cols {
            self.newline();
        }
        let idx = self.cursor_row * self.cols + self.cursor_col;
        self.cells[idx] = Cell { ch: c, fg: self.cur_fg, bg: self.cur_bg };
        self.cursor_col += 1;
    }

    // Same cost shape as TerminalGrid::scroll_up(): shifts the *entire*
    // buffer up by one row (not a ring-buffer index rotation), since
    // that's what the C++ side being compared against actually does.
    fn scroll_up(&mut self) {
        let cols = self.cols;
        self.cells.copy_within(cols.., 0);
        let last_row_start = (self.rows - 1) * cols;
        for c in &mut self.cells[last_row_start..] {
            *c = Cell::default();
        }
    }

    fn newline(&mut self) {
        self.cursor_col = 0;
        if self.cursor_row + 1 >= self.rows {
            self.scroll_up();
        } else {
            self.cursor_row += 1;
        }
    }

    fn clear_screen(&mut self) {
        for c in &mut self.cells {
            *c = Cell::default();
        }
    }

    fn clear_line_from_cursor(&mut self) {
        let row_start = self.cursor_row * self.cols;
        for c in &mut self.cells[row_start + self.cursor_col..row_start + self.cols] {
            *c = Cell::default();
        }
    }
}

impl Perform for Grid {
    fn print(&mut self, c: char) {
        self.write_char(c);
    }

    fn execute(&mut self, byte: u8) {
        match byte {
            b'\n' => self.newline(),
            b'\r' => self.cursor_col = 0,
            _ => {}
        }
    }

    fn csi_dispatch(&mut self, params: &Params, _intermediates: &[u8], _ignore: bool, action: char) {
        let mut it = params.iter();
        let next = |it: &mut ParamsIter<'_>| -> i64 {
            it.next().and_then(|p| p.first()).copied().unwrap_or(0) as i64
        };

        match action {
            'H' | 'f' => {
                let row = (next(&mut it).max(1) - 1) as usize;
                let col = (next(&mut it).max(1) - 1) as usize;
                self.cursor_row = row.min(self.rows.saturating_sub(1));
                self.cursor_col = col.min(self.cols.saturating_sub(1));
            }
            'A' => self.cursor_row = self.cursor_row.saturating_sub(next(&mut it).max(1) as usize),
            'B' => self.cursor_row = (self.cursor_row + next(&mut it).max(1) as usize).min(self.rows - 1),
            'C' => self.cursor_col = (self.cursor_col + next(&mut it).max(1) as usize).min(self.cols - 1),
            'D' => self.cursor_col = self.cursor_col.saturating_sub(next(&mut it).max(1) as usize),
            'J' => self.clear_screen(),
            'K' => self.clear_line_from_cursor(),
            'm' => {
                // 16/256/truecolor SGR, matching the subset sink's parser
                // and this benchmark's workloads actually exercise.
                let raw: Vec<i64> = params.iter().map(|p| p.first().copied().unwrap_or(0) as i64).collect();
                let mut i = 0;
                if raw.is_empty() {
                    self.cur_fg = (230, 230, 230);
                    self.cur_bg = (0, 0, 0);
                }
                while i < raw.len() {
                    let p = raw[i];
                    match p {
                        0 => { self.cur_fg = (230, 230, 230); self.cur_bg = (0, 0, 0); }
                        30..=37 => self.cur_fg = ansi16(p - 30, false),
                        90..=97 => self.cur_fg = ansi16(p - 90, true),
                        40..=47 => self.cur_bg = ansi16(p - 40, false),
                        100..=107 => self.cur_bg = ansi16(p - 100, true),
                        38 if i + 1 < raw.len() && raw[i + 1] == 2 && i + 4 < raw.len() => {
                            self.cur_fg = (raw[i + 2] as u8, raw[i + 3] as u8, raw[i + 4] as u8);
                            i += 4;
                        }
                        38 if i + 1 < raw.len() && raw[i + 1] == 5 && i + 2 < raw.len() => {
                            self.cur_fg = ansi256(raw[i + 2] as u8);
                            i += 2;
                        }
                        1 | 4 => {}
                        _ => {}
                    }
                    i += 1;
                }
            }
            _ => {}
        }
    }
}

fn ansi16(idx: i64, bright: bool) -> (u8, u8, u8) {
    let base: [(u8, u8, u8); 8] = [
        (13, 13, 13), (217, 38, 38), (38, 217, 38), (217, 191, 38),
        (38, 38, 217), (217, 38, 217), (38, 217, 217), (217, 217, 217),
    ];
    let mut c = base[(idx as usize) % 8];
    if bright {
        c = (c.0.saturating_add(70), c.1.saturating_add(70), c.2.saturating_add(70));
    }
    c
}

fn ansi256(idx: u8) -> (u8, u8, u8) {
    if idx < 16 {
        return ansi16(idx as i64 % 8, idx >= 8);
    }
    if idx < 232 {
        let n = idx - 16;
        let levels = [n / 36, (n / 6) % 6, n % 6];
        let conv = |l: u8| if l == 0 { 0 } else { l * 40 + 55 };
        return (conv(levels[0]), conv(levels[1]), conv(levels[2]));
    }
    let v = 8 + 10 * (idx - 232);
    (v, v, v)
}

fn run_once(data: &[u8], cols: usize, rows: usize) -> f64 {
    let mut grid = Grid::new(cols, rows);
    let mut parser = Parser::new();

    let start = Instant::now();
    for chunk in data.chunks(8192) {
        parser.advance(&mut grid, chunk);
    }
    start.elapsed().as_secs_f64()
}

fn main() {
    let dir = env::args().nth(1).unwrap_or_else(|| "bench/workloads".to_string());
    let workloads = [
        ("plain_text.bin", "plain text (cat-like)"),
        ("colored_text.bin", "SGR-heavy (colorized ls-like)"),
        ("cursor_heavy.bin", "cursor-heavy (TUI redraw-like)"),
        ("unicode_heavy.bin", "UTF-8 heavy (emoji/CJK/box-drawing)"),
    ];

    println!("{:<32} {:>10} {:>14}", "workload", "MB/s", "best time (s)");
    println!("{:<32} {:>10} {:>14}", "--------", "----", "-------------");

    for (file, label) in workloads {
        let path = format!("{dir}/{file}");
        let data = fs::read(&path).unwrap_or_else(|e| panic!("could not read {path}: {e}"));
        let mb = data.len() as f64 / (1024.0 * 1024.0);

        let mut best = f64::MAX;
        for _ in 0..5 {
            let t = run_once(&data, 120, 50);
            if t < best {
                best = t;
            }
        }

        println!("{:<32} {:>10.1} {:>14.4}", label, mb / best, best);
    }
}
