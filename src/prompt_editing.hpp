#pragma once

// Mouse editing of the shell's command line.
//
// sink lets a click place the shell's cursor, and a selection be deleted, as
// if the command line were a text field. The shell knows nothing about this:
// sink works out where on screen the line being edited sits, and translates a
// click into the cursor keys the shell would have received had the user
// pressed them.
//
// Kept out of main.cpp so it is a pure function of the grid -- which is what
// lets tests/prompt_editing_e2e.cpp drive it against a real shell in a PTY,
// headlessly, through exactly the code the app runs.

#include <string>

class TerminalGrid;

// The grid tracks where the line being edited begins (its InputAnchor), and
// has to be told about input as main.cpp sends it: note_user_edit() for
// anything that edits the line -- keys, clicks, pastes, cuts --
// note_user_submit() for Enter, Ctrl-C and Ctrl-D, and note_output_idle()
// once each batch of shell output has been parsed.

namespace prompt_edit {

// Bytes to write to the shell so its cursor lands on the cell at (row, col),
// in viewport coordinates (as the mouse reports them, scrollback offset
// included). Empty when the click is not on the line being edited, or the
// cursor is already there.
std::string click_payload(const TerminalGrid& grid, int row, int col);

// Bytes that delete the grid's current selection from the line being edited.
// Returns false, leaving `out` untouched, when the selection is not wholly
// inside that line -- the caller then treats the key as an ordinary key.
bool delete_selection_payload(const TerminalGrid& grid, std::string& out);

// The text of the line being edited, prompt excluded, rows joined by the
// newlines in it. Empty when there is no line being edited.
std::string current_line_text(const TerminalGrid& grid);

// Ends the line being edited (TerminalGrid::note_user_submit) and returns its
// text as it was. In that order on purpose: once submitted, where the line
// was is forgotten, so asking for its text afterwards gets nothing -- which is
// how the built-in commands (sinkdemo, sinksing) stopped being recognised when
// Enter marked the line submitted before main.cpp looked at it.
std::string submit_line(TerminalGrid& grid);

} // namespace prompt_edit
