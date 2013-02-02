#from gui_tools import *
from gi.repository import GObject, Gdk, Gtk, GooCanvas
import gui_tools

PPQN = 48

def standard_filter(patterns, name):
    f = Gtk.FileFilter()
    for p in patterns:
        f.add_pattern(p)
    f.set_name(name)
    return f

def hide_item(citem):
    citem.visibility = GooCanvas.CanvasItemVisibility.HIDDEN

def show_item(citem):
    citem.visibility = GooCanvas.CanvasItemVisibility.VISIBLE

def polygon_to_path(points):
    assert len(points) % 2 == 0, "Invalid number of points (%d) in %s" % (len(points), repr(points))
    path = ""
    if len(points) > 0:
        path += "M %s %s" % (points[0], points[1])
        if len(points) > 1:
            for i in range(2, len(points), 2):
                path += " L %s %s" % (points[i], points[i + 1])
    return path

class NoteModel(object):
    def __init__(self, pos, channel, row, vel, len = 1):
        self.pos = int(pos)
        self.channel = int(channel)
        self.row = int(row)
        self.vel = int(vel)
        self.len = int(len)
        self.item = None
        self.selected = False
    def __str__(self):
        return "pos=%s row=%s vel=%s len=%s" % (self.pos, self.row, self.vel, self.len)

# This is stupid and needs rewriting using a faster data structure
class DrumPatternModel(GObject.GObject):
    def __init__(self, beats, bars):
        GObject.GObject.__init__(self)
        self.ppqn = PPQN
        self.beats = beats
        self.bars = bars
        self.notes = []
        
    def import_data(self, data):
        self.clear()
        active_notes = {}
        for t in data:
            cmd = t[1] & 0xF0
            if len(t) == 4 and (cmd == 0x90) and (t[3] > 0):
                note = NoteModel(t[0], (t[1] & 15) + 1, t[2], t[3])
                active_notes[t[2]] = note
                self.add_note(note)
            if len(t) == 4 and ((cmd == 0x90 and t[3] == 0) or cmd == 0x80):
                if t[2] in active_notes:
                    active_notes[t[2]].len = t[0] - active_notes[t[2]].pos
                    del active_notes[t[2]]
        end = self.get_length()
        for n in active_notes.values():
            n.len = end - n.pos

    def clear(self):
        self.notes = []
        self.changed()

    def add_note(self, note, send_changed = True):
        self.notes.append(note)
        if send_changed:
            self.changed()
        
    def remove_note(self, pos, row, channel):
        self.notes = [note for note in self.notes if note.pos != pos or note.row != row or (channel is not None and note.channel != channel)]
        self.changed()
        
    def set_note_vel(self, note, vel):
        note.vel = int(vel)
        self.changed()
            
    def set_note_len(self, note, len):
        note.len = int(len)
        self.changed()
            
    def has_note(self, pos, row, channel):
        for n in self.notes:
            if n.pos == pos and n.row == row and (channel is None or n.channel == channel):
                return True
        return False
            
    def get_note(self, pos, row, channel):
        for n in self.notes:
            if n.pos == pos and n.row == row and (channel is None or n.channel == channel):
                return n
        return None
            
    def items(self):
        return self.notes
        
    def get_length(self):
        return self.beats * self.bars * self.ppqn
        
    def changed(self):
        self.emit('changed')

    def delete_selected(self):
        self.notes = [note for note in self.notes if not note.selected]
        self.changed()
        
    def group_set(self, **vals):
        for n in self.notes:
            if not n.selected:
                continue
            for k, v in vals.iteritems():
                setattr(n, k, v)
        self.changed()

    def transpose_selected(self, amount):
        for n in self.notes:
            if not n.selected:
                continue
            if n.row + amount < 0 or n.row + amount > 127:
                continue
            n.row += amount
        self.changed()

GObject.type_register(DrumPatternModel)
GObject.signal_new("changed", DrumPatternModel, GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE, ())

channel_ls = Gtk.ListStore(GObject.TYPE_STRING, GObject.TYPE_INT)
for ch in range(1, 17):
    channel_ls.append((str(ch), ch))
snap_settings_ls = Gtk.ListStore(GObject.TYPE_STRING, GObject.TYPE_INT)
for row in [("1/4", PPQN), ("1/8", PPQN // 2), ("1/8T", PPQN//3), ("1/16", PPQN//4), ("1/16T", PPQN//6), ("1/32", PPQN//8), ("1/32T", PPQN//12), ("1/64", PPQN//4)]:
    snap_settings_ls.append(row)
edit_mode_ls = Gtk.ListStore(GObject.TYPE_STRING, GObject.TYPE_STRING)
for row in [("Drum", "D"), ("Melodic", "M")]:
    edit_mode_ls.append(row)

class DrumEditorToolbox(Gtk.HBox):
    def __init__(self, canvas):
        Gtk.HBox.__init__(self, spacing = 5)
        self.canvas = canvas
        self.vel_adj = Gtk.Adjustment(100, 1, 127, 1, 10, 0)
        
        self.pack_start(Gtk.Label("Channel:"), False, False, 0)
        self.channel_setting = gui_tools.standard_combo(channel_ls, active_item_lookup = self.canvas.channel, lookup_column = 1)
        self.channel_setting.connect('changed', lambda w: self.canvas.set_channel(w.get_model()[w.get_active()][1]))
        self.pack_start(self.channel_setting, False, True, 0)
        
        self.pack_start(Gtk.Label("Mode:"), False, False, 0)
        self.mode_setting = gui_tools.standard_combo(edit_mode_ls, active_item_lookup = self.canvas.edit_mode, lookup_column = 1)
        self.mode_setting.connect('changed', lambda w: self.canvas.set_edit_mode(w.get_model()[w.get_active()][1]))
        self.pack_start(self.mode_setting, False, True, 0)
        
        self.pack_start(Gtk.Label("Snap:"), False, False, 0)
        self.snap_setting = gui_tools.standard_combo(snap_settings_ls, active_item_lookup = self.canvas.grid_unit, lookup_column = 1)
        self.snap_setting.connect('changed', lambda w: self.canvas.set_grid_unit(w.get_model()[w.get_active()][1]))
        self.pack_start(self.snap_setting, False, True, 0)
        
        self.pack_start(Gtk.Label("Velocity:"), False, False, 0)
        self.pack_start(Gtk.SpinButton(adjustment = self.vel_adj, climb_rate = 0, digits = 0), False, False, 0)
        button = Gtk.Button("Load")
        button.connect('clicked', self.load_pattern)
        self.pack_start(button, True, True, 0)
        button = Gtk.Button("Save")
        button.connect('clicked', self.save_pattern)
        self.pack_start(button, True, True, 0)
        button = Gtk.Button("Double")
        button.connect('clicked', self.double_pattern)
        self.pack_start(button, True, True, 0)
        self.pack_start(Gtk.Label("--"), False, False, 0)

    def update_edit_mode(self):
        self.mode_setting.set_active(gui_tools.ls_index(edit_mode_ls, self.canvas.edit_mode, 1))

    def load_pattern(self, w):
        dlg = Gtk.FileChooserDialog('Open a drum pattern', self.get_toplevel(), Gtk.FileChooserAction.OPEN,
            (Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL, Gtk.STOCK_OPEN, Gtk.ResponseType.APPLY))
        dlg.add_filter(standard_filter(["*.cbdp"], "Drum patterns"))
        dlg.add_filter(standard_filter(["*"], "All files"))
        try:
            if dlg.run() == Gtk.ResponseType.APPLY:
                pattern = self.canvas.pattern
                f = file(dlg.get_filename(), "r")
                pattern.clear()
                pattern.beats, pattern.bars = [int(v) for v in f.readline().strip().split(";")]
                for line in f.readlines():
                    line = line.strip()
                    if not line.startswith("n:"):
                        pos, row, vel = line.split(";")
                        row = int(row) + 36
                        channel = 10
                        len = 1
                    else:
                        pos, channel, row, vel, len = line[2:].split(";")
                    self.canvas.pattern.add_note(NoteModel(pos, channel, row, vel, len), send_changed = False)
                f.close()
                self.canvas.pattern.changed()
                self.canvas.update_grid()
                self.canvas.update_notes()
        finally:
            dlg.destroy()
    def save_pattern(self, w):
        dlg = Gtk.FileChooserDialog('Save a drum pattern', self.get_toplevel(), Gtk.FileChooserAction.SAVE, 
            (Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL, Gtk.STOCK_SAVE, Gtk.ResponseType.APPLY))
        dlg.add_filter(standard_filter(["*.cbdp"], "Drum patterns"))
        dlg.add_filter(standard_filter(["*"], "All files"))
        dlg.set_current_name("pattern.cbdp")
        try:
            if dlg.run() == Gtk.ResponseType.APPLY:            
                pattern = self.canvas.pattern
                f = file(dlg.get_filename(), "w")
                f.write("%s;%s\n" % (pattern.beats, pattern.bars))
                for i in self.canvas.pattern.items():
                    f.write("n:%s;%s;%s;%s;%s\n" % (i.pos, i.channel, i.row, i.vel, i.len))
                f.close()
        finally:
            dlg.destroy()
    def double_pattern(self, w):
        len = self.canvas.pattern.get_length()
        self.canvas.pattern.bars *= 2
        new_notes = []
        for note in self.canvas.pattern.items():
            new_notes.append(NoteModel(note.pos + len, note.channel, note.row, note.vel, note.len))
        for note in new_notes:
            self.canvas.pattern.add_note(note, send_changed = False)
        self.canvas.pattern.changed()
        self.canvas.update_size()
        self.canvas.update_grid()
        self.canvas.update_notes()

class DrumCanvasCursor(object):
    def __init__(self, canvas):
        self.canvas = canvas
        self.canvas_root = canvas.get_root_item()
        self.frame = GooCanvas.CanvasRect(parent = self.canvas_root, x = -6, y = -6, width = 12, height = 12 , stroke_color = "gray", line_width = 1)
        hide_item(self.frame)
        self.vel = GooCanvas.CanvasText(parent = self.canvas_root, x = 0, y = 0, fill_color = "blue", stroke_color = "blue", anchor = GooCanvas.CanvasAnchorType.S)
        hide_item(self.vel)
        self.rubberband = False
        self.rubberband_origin = None
        self.rubberband_current = None

    def hide(self):
        hide_item(self.frame)
        hide_item(self.vel)
        
    def show(self):
        show_item(self.frame)
        show_item(self.vel)
        
    def move_to_note(self, note):
        self.move(note.pos, note.row, note)
            
    def move(self, pulse, row, note):
        x = self.canvas.pulse_to_screen_x(pulse)
        y = self.canvas.row_to_screen_y(row) + self.canvas.row_height / 2
        dx = 0
        if note is not None:
            dx = self.canvas.pulse_to_screen_x(pulse + note.len) - x
        self.frame.set_properties(x = x - 6, width = 12 + dx, y = y - 6, height = 12)
        cy = y - self.canvas.row_height * 1.5 if y >= self.canvas.rows * self.canvas.row_height / 2 else y + self.canvas.row_height * 1.5
        if note is None:
            text = ""
        else:
            text = "[%s] %s" % (note.channel, note.vel)
        self.vel.set_properties(x = x, y = cy, text = text)
        
    def start_rubberband(self, x, y):
        self.rubberband = True
        self.rubberband_origin = (x, y)
        self.rubberband_current = (x, y)
        self.update_rubberband_frame()
        show_item(self.frame)
        
    def update_rubberband(self, x, y):
        self.rubberband_current = (x, y)
        self.update_rubberband_frame()
        
    def end_rubberband(self, x, y):
        self.rubberband_current = (x, y)
        self.update_rubberband_frame()
        hide_item(self.frame)
        self.rubberband = False
        
    def cancel_rubberband(self):
        hide_item(self.frame)
        self.rubberband = False
        
    def update_rubberband_frame(self):
        self.frame.set_properties(x = self.rubberband_origin[0],
            y = self.rubberband_origin[1],
            width = self.rubberband_current[0] - self.rubberband_origin[0],
            height = self.rubberband_current[1] - self.rubberband_origin[1])
        
    def get_rubberband_box(self):
        x1, y1 = self.rubberband_origin
        x2, y2 = self.rubberband_current
        return (min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2))

class DrumCanvas(GooCanvas.Canvas):
    def __init__(self, rows, pattern):
        GooCanvas.Canvas.__init__(self)
        self.rows = rows
        self.pattern = pattern
        self.row_height = 24
        self.grid_unit = PPQN / 4 # unit in pulses
        self.zoom_in = 2
        self.instr_width = 120
        self.edited_note = None
        self.orig_velocity = None
        self.orig_length = None
        self.orig_y = None
        self.channel_modes = ['D' if ch == 10 else 'M' for ch in range(1, 17)]
        
        self.update_size()
        
        self.grid = GooCanvas.CanvasGroup(parent = self.get_root_item(), x = self.instr_width)
        self.update_grid()

        self.notes = GooCanvas.CanvasGroup(parent = self.get_root_item(), x = self.instr_width)

        self.names = GooCanvas.CanvasGroup(parent = self.get_root_item(), x = 0)
        self.update_names()
        
        self.cursor = DrumCanvasCursor(self)
        hide_item(self.cursor)
        
        self.connect('event', self.on_grid_event)

        self.channel = 10
        self.edit_mode = self.channel_modes[self.channel - 1]
        self.toolbox = DrumEditorToolbox(self)

        self.add_events(Gdk.EventMask.POINTER_MOTION_HINT_MASK)

        self.grab_focus(self.grid)
        self.update_notes()

    def set_edit_mode(self, mode):
        self.edit_mode = mode
        self.channel_modes[self.channel - 1] = mode
        
    def calc_size(self):
        return (self.instr_width + self.pattern.get_length() * self.zoom_in + 1, self.rows * self.row_height + 1)
        
    def set_grid_unit(self, grid_unit):
        self.grid_unit = grid_unit
        self.update_grid()
        
    def set_channel(self, channel):
        self.channel = channel
        self.set_edit_mode(self.channel_modes[self.channel - 1])
        self.update_notes()
        self.toolbox.update_edit_mode()
        
    def update_size(self):
        sx, sy = self.calc_size()
        self.set_bounds(0, 0, sx, self.rows * self.row_height)
        self.set_size_request(sx, sy)
    
    def update_names(self):
        for i in self.names.items:
            i.destroy()
        for i in range(0, self.rows):
            #GooCanvas.CanvasText(parent = self.names, text = gui_tools.note_to_name(i), x = self.instr_width - 10, y = (i + 0.5) * self.row_height, anchor = Gtk.AnchorType.E, size_points = 10, font = "Sans", size_set = True)
            GooCanvas.CanvasText(parent = self.names, text = gui_tools.note_to_name(i), x = self.instr_width - 10, y = (i + 0.5) * self.row_height, anchor = GooCanvas.CanvasAnchorType.E, font = "Sans")
        
    def update_grid(self):
        for i in self.grid.items:
            i.destroy()
        bg = GooCanvas.CanvasRect(parent = self.grid, x = 0, y = 0, width = self.pattern.get_length() * self.zoom_in, height = self.rows * self.row_height, fill_color = "white")
        bar_fg = "blue"
        beat_fg = "darkgray"
        grid_fg = "lightgray"
        row_grid_fg = "lightgray"
        row_ext_fg = "black"
        for i in range(0, self.rows + 1):
            color = row_ext_fg if (i == 0 or i == self.rows) else row_grid_fg
            GooCanvas.CanvasPath(parent = self.grid, data = "M %s %s L %s %s" % (0, i * self.row_height, self.pattern.get_length() * self.zoom_in, i * self.row_height), stroke_color = color, line_width = 1)
        for i in range(0, self.pattern.get_length() + 1, self.grid_unit):
            color = grid_fg
            if i % self.pattern.ppqn == 0:
                color = beat_fg
            if (i % (self.pattern.ppqn * self.pattern.beats)) == 0:
                color = bar_fg
            GooCanvas.CanvasPath(parent = self.grid, data = "M %s %s L %s %s" % (i * self.zoom_in, 1, i * self.zoom_in, self.rows * self.row_height - 1), stroke_color = color, line_width = 1)
            
    def update_notes(self):
        while self.notes.get_n_children() > 0:
            self.notes.remove_child(0)
        for item in self.pattern.items():
            x = self.pulse_to_screen_x(item.pos) - self.instr_width
            y = self.row_to_screen_y(item.row + 0.5)
            if item.channel == self.channel:
                fill_color = 0xC0C0C0FF - int(item.vel * 1.5) * 0x00010100
                stroke_color = 0x808080FF
                if item.selected:
                    stroke_color = 0xFF8080FF
            else:
                fill_color = 0xE0E0E0FF
                stroke_color = 0xE0E0E0FF
            if item.len > 1:
                x2 = self.pulse_to_screen_x(item.pos + item.len) - self.pulse_to_screen_x(item.pos)
                polygon = [-2, 0, 0, -5, x2 - 5, -5, x2, 0, x2 - 5, 5, 0, 5]
            else:
                polygon = [-4, 0, 0, -5, 5, 0, 0, 5]
            item.item = GooCanvas.CanvasPath(parent = self.notes, data = polygon_to_path(polygon), fill_color_rgba = fill_color, stroke_color_rgba = stroke_color, line_width = 1)
            item.item.translate(x, y)

    def set_selection_from_rubberband(self):
        sx, sy, ex, ey = self.cursor.get_rubberband_box()
        for item in self.pattern.items():
            x = self.pulse_to_screen_x(item.pos)
            y = self.row_to_screen_y(item.row + 0.5)
            item.selected = (x >= sx and x <= ex and y >= sy and y <= ey)
        self.update_notes()

    def on_grid_event(self, item, event):
        if event.type == Gdk.EventType.KEY_PRESS:
            return self.on_key_press(item, event)
        if event.type in [Gdk.EventType.BUTTON_PRESS, Gdk.EventType._2BUTTON_PRESS, Gdk.EventType.LEAVE_NOTIFY, Gdk.EventType.MOTION_NOTIFY, Gdk.EventType.BUTTON_RELEASE]:
            return self.on_mouse_event(item, event)

    def on_key_press(self, item, event):
        keyval, state = event.keyval, event.state
        kvname = Gdk.keyval_name(keyval)
        if kvname == 'Delete':
            self.pattern.delete_selected()
            self.update_notes()
        elif kvname == 'c':
            self.pattern.group_set(channel = self.channel)
            self.update_notes()
        elif kvname == 'v':
            self.pattern.group_set(vel = int(self.toolbox.vel_adj.get_value()))
            self.update_notes()
        elif kvname == 'plus':
            self.pattern.transpose_selected(1)
            self.update_notes()
        elif kvname == 'minus':
            self.pattern.transpose_selected(-1)
            self.update_notes()
        #else:
        #    print kvname

    def on_mouse_event(self, item, event):
        ex, ey = self.convert_to_item_space(self.get_root_item(), event.x, event.y)
        column = self.screen_x_to_column(ex)
        row = self.screen_y_to_row(ey)
        pulse = column * self.grid_unit
        epulse = (ex - self.instr_width) / self.zoom_in
        unit = self.grid_unit * self.zoom_in
        if self.cursor.rubberband:
            if event.type == Gdk.EventType.MOTION_NOTIFY:
                self.cursor.update_rubberband(ex, ey)
                return
            button = event.get_button()[1]
            if event.type == Gdk.EventType.BUTTON_RELEASE and button == 1:
                self.cursor.end_rubberband(ex, ey)
                self.set_selection_from_rubberband()
                self.request_update()
                return
            return
        if event.type == Gdk.EventType.BUTTON_PRESS:
            button = event.get_button()[1]
            self.grab_focus(self.grid)
            if ((event.state & Gdk.ModifierType.SHIFT_MASK) == Gdk.ModifierType.SHIFT_MASK) and button == 1:
                self.cursor.start_rubberband(ex, ey)
                return
            if pulse < 0 or pulse >= self.pattern.get_length():
                return
            note = self.pattern.get_note(pulse, row, self.channel)
            if note is not None:
                if button == 3:
                    vel = int(self.toolbox.vel_adj.get_value())
                    self.pattern.set_note_vel(note, vel)
                    self.cursor.move(pulse, row, note)
                    self.update_notes()
                    return
                self.toolbox.vel_adj.set_value(note.vel)
            else:
                note = NoteModel(pulse, self.channel, row, int(self.toolbox.vel_adj.get_value()), self.grid_unit if self.edit_mode == 'M' else 1)
                self.pattern.add_note(note)
            self.edited_note = note
            self.orig_length = note.len
            self.orig_velocity = note.vel
            self.orig_y = ey
            self.grab_add()
            self.cursor.move(self.edited_note.pos, self.edited_note.row, note)
            self.update_notes()
            return
        if event.type == Gdk.EventType._2BUTTON_PRESS:
            if pulse < 0 or pulse >= self.pattern.get_length():
                return
            if self.pattern.has_note(pulse, row, self.channel):
                self.pattern.remove_note(pulse, row, self.channel)
            self.cursor.move(pulse, row, None)
            self.update_notes()
            if self.edited_note is not None:
                self.grab_remove()
                self.edited_note = None
            return
        if event.type == Gdk.EventType.LEAVE_NOTIFY and self.edited_note is None:
            hide_item(self.cursor)
            return
        if event.type == Gdk.EventType.MOTION_NOTIFY and self.edited_note is None:
            if pulse < 0 or pulse >= self.pattern.get_length():
                hide_item(self.cursor)
                return
            
            if abs(pulse - epulse) > 5:
                hide_item(self.cursor)
                return
            note = self.pattern.get_note(column * self.grid_unit, row, self.channel)
            self.cursor.move(pulse, row, note)
            show_item(self.cursor)
            return
        if event.type == Gdk.EventType.MOTION_NOTIFY and self.edited_note is not None:
            vel = int(self.orig_velocity - self.snap(ey - self.orig_y) / 2)
            if vel < 1: vel = 1
            if vel > 127: vel = 127
            self.pattern.set_note_vel(self.edited_note, vel)
            len = pulse - self.edited_note.pos
            if self.edit_mode == 'D':
                len = 1
            elif len <= -self.grid_unit:
                len = self.orig_length
            elif len <= self.grid_unit:
                len = self.grid_unit
            else:
                len = int((len + 1) /  self.grid_unit) * self.grid_unit - 1
            self.pattern.set_note_len(self.edited_note, len)
            self.toolbox.vel_adj.set_value(vel)
            self.cursor.move_to_note(self.edited_note)
            self.update_notes()
            self.request_update()
            return
        if event.type == Gdk.EventType.BUTTON_RELEASE and self.edited_note is not None:
            self.edited_note = None
            self.grab_remove()
            return
            
    def screen_x_to_column(self, x):
        unit = self.grid_unit * self.zoom_in
        return int((x - self.instr_width + unit / 2) / unit)
        
    def screen_y_to_row(self, y):
        return int((y - 1) / self.row_height)
        
    def pulse_to_screen_x(self, pulse):
        return pulse * self.zoom_in + self.instr_width
        
    def column_to_screen_x(self, column):
        unit = self.grid_unit * self.zoom_in
        return column * unit + self.instr_width
        
    def row_to_screen_y(self, row):
        return row * self.row_height + 1
        
    def snap(self, val):
        if val > -10 and val < 10:
            return 0
        if val >= 10:
            return val - 10
        if val <= -10:
            return val + 10
        assert False

class DrumSeqWindow(Gtk.Window):
    def __init__(self, length, pat_data):
        Gtk.Window.__init__(self, Gtk.WindowType.TOPLEVEL)
        self.vbox = Gtk.VBox(spacing = 5)
        self.pattern = DrumPatternModel(4, length / (4 * PPQN))
        if pat_data is not None:
            self.pattern.import_data(pat_data)

        self.canvas = DrumCanvas(128, self.pattern)
        sw = Gtk.ScrolledWindow()
        sw.set_size_request(640, 400)
        sw.add_with_viewport(self.canvas)
        self.vbox.pack_start(sw, True, True, 0)
        self.vbox.pack_start(self.canvas.toolbox, False, False, 0)
        self.add(self.vbox)

if __name__ == "__main__":
    w = DrumSeqWindow()
    w.set_title("Drum pattern editor")
    w.show_all()
    w.connect('destroy', lambda w: Gtk.main_quit())

    Gtk.main()
