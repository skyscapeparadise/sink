// Headless throughput benchmark for the `vte` crate -- the actual VT
// parser Alacritty is built on. Reads the exact same workload files
// bench/sink_bench.cpp does and times vte::Parser::advance() over them.
//
// The Perform impl below deliberately does *comparable* bookkeeping to
// sink's TerminalGrid: a flat cell buffer, cursor tracking, SGR colour
// state, and a row ring so that a full-screen scroll rotates a base index
// instead of moving the buffer. The point of this benchmark is to isolate
// "parsing + basic cell-write cost" as fairly as possible -- giving either
// side a smarter grid would measure grid architecture, not parser speed,
// and conflate the two.
//
// This originally shifted the whole buffer on scroll, matching what sink's
// grid did at the time. When sink moved to a ring, that parity broke and
// the comparison silently started flattering sink on scroll-heavy
// workloads, so the ring was mirrored here too. Real Alacritty also uses a
// ring, so this is closer to it than the original was.
//
// Double-width handling is mirrored here too, for the same reason as the ring:
// real Alacritty gives East Asian Wide characters and emoji two columns, so a
// harness that gave them one would be measuring sink doing correct work
// against a stand-in doing less of it. The UTF-8 workload is 23% wide
// characters, which is a 1.23x difference in cells written -- easily enough to
// decide that comparison on its own.
//
// Two differences remain and both favour this side: Cell here is 12 bytes
// against sink's 20, because sink's cells also carry SGR attribute bits and a
// hyperlink id; and sink additionally checks every non-ASCII character for
// combining marks to compose them onto their base, which this does not do.
// Both are real costs of sink's feature set rather than benchmark artifacts,
// so they are left alone rather than padded to match.
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

/// Columns a character occupies. Mirrors char_display_width() in
/// src/terminal_grid.cpp: East Asian Wide and Fullwidth plus the emoji blocks
/// with Emoji_Presentation. Box-drawing is Ambiguous and stays narrow.
#[inline]
fn char_width(c: char) -> usize {
    let cp = c as u32;
    if cp < 0x1100 {
        return 1;
    }
    if (0x1100..=0x115F).contains(&cp)
        || (0x2E80..=0x303E).contains(&cp)
        || (0x3041..=0x33FF).contains(&cp)
        || (0x3400..=0x4DBF).contains(&cp)
        || (0x4E00..=0x9FFF).contains(&cp)
        || (0xA000..=0xA4CF).contains(&cp)
        || (0xA960..=0xA97F).contains(&cp)
        || (0xAC00..=0xD7A3).contains(&cp)
        || (0xF900..=0xFAFF).contains(&cp)
        || (0xFE10..=0xFE19).contains(&cp)
        || (0xFE30..=0xFE6F).contains(&cp)
        || (0xFF00..=0xFF60).contains(&cp)
        || (0xFFE0..=0xFFE6).contains(&cp)
        || (0x1F300..=0x1F64F).contains(&cp)
        || (0x1F680..=0x1F6FF).contains(&cp)
        || (0x1F900..=0x1F9FF).contains(&cp)
        || (0x1FA70..=0x1FAFF).contains(&cp)
        || (0x20000..=0x2FFFD).contains(&cp)
        || (0x30000..=0x3FFFD).contains(&cp)
    {
        return 2;
    }
    1
}

/// Variation selectors, ZWJ/ZWNJ and the basic combining-mark block: the
/// zero-width characters the workloads contain.
#[inline]
fn is_zero_width(c: char) -> bool {
    matches!(c as u32, 0x0300..=0x036F | 0x200B..=0x200D | 0xFE00..=0xFE0F)
}

struct Grid {
    cols: usize,
    rows: usize,
    cells: Vec<Cell>,
    /// Ring base: logical row r lives at physical row phys_row(r). Mirrors
    /// TerminalGrid::row_base_ on the C++ side.
    row_base: usize,
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
            row_base: 0,
            cursor_row: 0,
            cursor_col: 0,
            cur_fg: (230, 230, 230),
            cur_bg: (0, 0, 0),
        }
    }

    /// row_base and r are both < rows, so their sum is below 2 * rows and a
    /// conditional subtract replaces the modulo.
    #[inline]
    fn phys_row(&self, r: usize) -> usize {
        let p = r + self.row_base;
        if p >= self.rows { p - self.rows } else { p }
    }

    #[inline]
    fn row_start(&self, r: usize) -> usize {
        self.phys_row(r) * self.cols
    }

    fn write_char(&mut self, c: char) {
        // Zero-width characters attach to the cell before them, as in real
        // Alacritty (and sink, which composes them into the base). Writing
        // nothing is cheaper than Alacritty's attach, so this errs in vte's
        // favour. Without it the UTF-8 workload's U+FE0F took a column of its
        // own and the two sides ended on different screens.
        if is_zero_width(c) {
            return;
        }
        let w = char_width(c);
        // A double-width glyph cannot straddle a line break.
        if self.cursor_col + w > self.cols {
            self.newline();
        }
        let idx = self.row_start(self.cursor_row) + self.cursor_col;
        self.cells[idx] = Cell { ch: c, fg: self.cur_fg, bg: self.cur_bg };
        if w == 2 && self.cursor_col + 1 < self.cols {
            // Trailing half: same style, no character of its own.
            self.cells[idx + 1] = Cell { ch: '\0', fg: self.cur_fg, bg: self.cur_bg };
        }
        self.cursor_col += w;
    }

    // Same cost shape as TerminalGrid::scroll_up(): rotate the ring base and
    // blank the row that falls off, rather than moving the buffer.
    fn scroll_up(&mut self) {
        self.row_base = self.phys_row(1);
        let start = self.row_start(self.rows - 1);
        let cols = self.cols;
        for c in &mut self.cells[start..start + cols] {
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
        let row_start = self.row_start(self.cursor_row);
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

/// FNV-1a over the visible screen as plain text, then the cursor. Mirrors
/// screen_text_hash() in bench/harness.hpp exactly, so the two benchmarks can
/// be checked for ending in the same state -- i.e. for having done the same
/// work. A blank cell or the trailing half of a wide character is a space. A
/// pending wrap leaves this cursor one past the last column where sink's
/// stays on it with a flag set; that is representation, not a difference in
/// the result, so the column is clamped.
fn screen_text_hash(g: &Grid) -> String {
    let mut h: u64 = 0xcbf29ce484222325;
    let mut put = |v: u32| {
        for i in 0..4 {
            h = (h ^ ((v >> (8 * i)) & 0xff) as u64).wrapping_mul(0x100000001b3);
        }
    };
    for r in 0..g.rows {
        let start = g.row_start(r);
        for c in 0..g.cols {
            let ch = g.cells[start + c].ch as u32;
            put(if ch == 0 { 32 } else { ch });
        }
    }
    put(g.cursor_col.min(g.cols - 1) as u32);
    put(g.cursor_row as u32);
    format!("{:016x}", h)
}

/// Everything in the grid, colours included. Nothing compares this across
/// the two sides (the colour models differ); it exists so the colour fields
/// are read. A field that is written and never read lets the optimizer drop
/// the writes, which would quietly make this side do less work than sink.
fn cells_hash(g: &Grid) -> String {
    let mut h: u64 = 0xcbf29ce484222325;
    for cell in &g.cells {
        for b in [cell.fg.0, cell.fg.1, cell.fg.2, cell.bg.0, cell.bg.1, cell.bg.2] {
            h = (h ^ b as u64).wrapping_mul(0x100000001b3);
        }
        h = (h ^ cell.ch as u64).wrapping_mul(0x100000001b3);
    }
    format!("{:016x}", h)
}

fn run_once(data: &[u8], cols: usize, rows: usize) -> (f64, String) {
    let mut grid = Grid::new(cols, rows);
    let mut parser = Parser::new();

    let start = Instant::now();
    for chunk in data.chunks(8192) {
        parser.advance(&mut grid, chunk);
    }
    let t = start.elapsed().as_secs_f64();
    (t, format!("{}:{}", screen_text_hash(&grid), cells_hash(&grid)))
}

fn median(v: &[f64]) -> f64 {
    let mut s = v.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let n = s.len();
    if n % 2 == 1 { s[n / 2] } else { 0.5 * (s[n / 2 - 1] + s[n / 2]) }
}

// Same flags and JSON shape as the C++ benchmarks (bench/harness.hpp):
//   vte_bench [--json] [--samples N] [--warmup N] [dir]
fn main() {
    let mut json = false;
    let mut samples = 20usize;
    let mut warmup = 3usize;
    let mut dir = "bench/workloads".to_string();
    let args: Vec<String> = env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => json = true,
            "--samples" => { i += 1; samples = args[i].parse().expect("--samples N"); }
            "--warmup" => { i += 1; warmup = args[i].parse().expect("--warmup N"); }
            other => dir = other.to_string(),
        }
        i += 1;
    }
    // Ids match sink_bench's parser_core.* metrics, the like-for-like ones.
    let workloads = [
        ("plain_text.bin", "plain_text", "plain text (cat-like)"),
        ("colored_text.bin", "colored_text", "SGR-heavy (colorized ls)"),
        ("cursor_heavy.bin", "cursor_heavy", "cursor-heavy (TUI redraw)"),
        ("unicode_heavy.bin", "unicode_heavy", "UTF-8 heavy (emoji/CJK/box)"),
    ];
    let (cols, rows) = (120usize, 50usize);

    let mut metrics = Vec::new();
    for (file, id, label) in workloads {
        let path = format!("{dir}/{file}");
        let data = fs::read(&path).unwrap_or_else(|e| panic!("could not read {path}: {e}"));
        let mb = data.len() as f64 / (1024.0 * 1024.0);
        let (_, screen) = run_once(&data, cols, rows);
        for _ in 0..warmup {
            run_once(&data, cols, rows);
        }
        let mut s = Vec::with_capacity(samples);
        for _ in 0..samples {
            s.push(mb / run_once(&data, cols, rows).0);
        }
        let (_, again) = run_once(&data, cols, rows);
        if again != screen {
            eprintln!("{id}: output differs between runs");
            std::process::exit(1);
        }
        let (screen, cells) = screen.split_once(':').map(|(a, b)| (a.to_string(), b.to_string())).unwrap();
        metrics.push((id, label, s, screen, cells, data.len()));
    }

    if json {
        println!("{{\n  \"bench\": \"vte_bench\",\n  \"config\": {{\"cols\": {cols}, \"rows\": {rows}, \"chunk_bytes\": 8192, \"samples\": {samples}, \"warmup\": {warmup}, \"vte\": \"0.15\"}},\n  \"metrics\": [");
        for (k, (id, label, s, screen, cells, bytes)) in metrics.iter().enumerate() {
            let list: Vec<String> = s.iter().map(|v| format!("{v:.9}")).collect();
            println!(
                "    {{\"id\": \"vte.{id}\", \"label\": \"vte: {label}\", \"unit\": \"MB/s\", \"better\": \"higher\",\n     \"fingerprint\": null,\n     \"info\": {{\"screen_text\": \"{screen}\", \"cells\": \"{cells}\", \"bytes\": {bytes}}},\n     \"samples\": [{}]}}{}",
                list.join(", "),
                if k + 1 < metrics.len() { "," } else { "" }
            );
        }
        println!("  ]\n}}");
    } else {
        println!("{:<38} {:>12} {:>18}", "workload", "median MB/s", "screen");
        for (_, label, s, screen, _, _) in &metrics {
            println!("{:<38} {:>12.1} {:>18}", label, median(s), screen);
        }
    }
}
