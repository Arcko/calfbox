import pygtk
import gtk
import glib
import gobject
import math
import sys

sys.path = ["./py"] + sys.path

import cbox
from gui_tools import *
import fx_gui
import instr_gui
import drumkit_editor
import drum_pattern_editor

class SceneDialog(SelectObjectDialog):
    title = "Select a scene"
    def __init__(self, parent):
        SelectObjectDialog.__init__(self, parent)
    def update_model(self, model):
        for s in cbox.Config.sections("scene:"):
            title = s["title"]
            model.append((s.name[6:], "Scene", s.name, title))
        for s in cbox.Config.sections("instrument:"):
            title = s["title"]
            model.append((s.name[11:], "Instrument", s.name, title))
        for s in cbox.Config.sections("layer:"):
            title = s["title"]
            model.append((s.name[6:], "Layer", s.name, title))

class AddLayerDialog(SelectObjectDialog):
    title = "Add a layer"
    def __init__(self, parent):
        SelectObjectDialog.__init__(self, parent)
    def update_model(self, model):
        for s in cbox.Config.sections("instrument:"):
            title = s["title"]
            model.append((s.name[11:], "Instrument", s.name, title))
        for s in cbox.Config.sections("layer:"):
            title = s["title"]
            model.append((s.name[6:], "Layer", s.name, title))

class PlayPatternDialog(SelectObjectDialog):
    title = "Play a drum pattern"
    def __init__(self, parent):
        SelectObjectDialog.__init__(self, parent)
    def update_model(self, model):
        model.append((None, "Stop", "", ""))
        for s in cbox.Config.sections("drumpattern:"):
            title = s["title"]
            model.append((s.name[12:], "Pattern", s.name, title))
        for s in cbox.Config.sections("drumtrack:"):
            title = s["title"]
            model.append((s.name[10:], "Track", s.name, title))

in_channels_ls = gtk.ListStore(gobject.TYPE_INT, gobject.TYPE_STRING)
in_channels_ls.append((0, "All"))
out_channels_ls = gtk.ListStore(gobject.TYPE_INT, gobject.TYPE_STRING)
out_channels_ls.append((0, "Same"))
for i in range(1, 17):
    in_channels_ls.append((i, str(i)))
    out_channels_ls.append((i, str(i)))
notes_ls = gtk.ListStore(gobject.TYPE_INT, gobject.TYPE_STRING)
opt_notes_ls = gtk.ListStore(gobject.TYPE_INT, gobject.TYPE_STRING)
opt_notes_ls.append((-1, "N/A"))
for i in range(0, 128):
    notes_ls.append((i, note_to_name(i)))
    opt_notes_ls.append((i, note_to_name(i)))
transpose_ls = gtk.ListStore(gobject.TYPE_INT, gobject.TYPE_STRING)
for i in range(-60, 61):
    transpose_ls.append((i, str(i)))

class SceneLayersModel(gtk.ListStore):
    def __init__(self):
        gtk.ListStore.__init__(self, gobject.TYPE_STRING, gobject.TYPE_BOOLEAN, 
            gobject.TYPE_INT, gobject.TYPE_INT, gobject.TYPE_BOOLEAN,
            gobject.TYPE_INT, gobject.TYPE_INT, gobject.TYPE_INT, gobject.TYPE_INT, gobject.TYPE_STRING)
    #def make_row_item(self, opath, tree_path):
    #    return opath % self[(1 + int(tree_path))]
    def make_row_item(self, opath, tree_path):
        return cbox.Document.uuid_cmd(self[int(tree_path)][-1], opath)
    def refresh(self, scene_status):
        self.clear()
        for layer in scene_status.layers:
            ls = layer.status()
            self.append((ls.instrument_name, ls.enable != 0, ls.in_channel, ls.out_channel, ls.consume != 0, ls.low_note, ls.high_note, ls.fixed_note, ls.transpose, layer.uuid))

class SceneLayersView(gtk.TreeView):
    def __init__(self, model):
        gtk.TreeView.__init__(self, model)
        self.enable_model_drag_source(gtk.gdk.BUTTON1_MASK, [("text/plain", 0, 1)], gtk.gdk.ACTION_MOVE)
        self.enable_model_drag_dest([("text/plain", gtk.TARGET_SAME_APP | gtk.TARGET_SAME_WIDGET, 1)], gtk.gdk.ACTION_MOVE)
        self.connect('drag_data_get', self.drag_data_get)
        self.connect('drag_data_received', self.drag_data_received)
        self.insert_column_with_attributes(0, "On?", standard_toggle_renderer(model, "/enable", 1), active=1)
        self.insert_column_with_attributes(1, "Name", gtk.CellRendererText(), text=0)
        self.insert_column_with_data_func(2, "In Ch#", standard_combo_renderer(model, in_channels_ls, "/in_channel", 2), lambda column, cell, model, iter: cell.set_property('text', "%s" % model[iter][2] if model[iter][2] != 0 else 'All'))
        self.insert_column_with_data_func(3, "Out Ch#", standard_combo_renderer(model, out_channels_ls, "/out_channel", 3), lambda column, cell, model, iter: cell.set_property('text', "%s" % model[iter][3] if model[iter][3] != 0 else 'Same'))
        self.insert_column_with_attributes(4, "Eat?", standard_toggle_renderer(model, "/consume", 4), active=4)
        self.insert_column_with_data_func(5, "Lo N#", standard_combo_renderer(model, notes_ls, "/low_note", 5), lambda column, cell, model, iter: cell.set_property('text', note_to_name(model[iter][5])))
        self.insert_column_with_data_func(6, "Hi N#", standard_combo_renderer(model, notes_ls, "/high_note", 6), lambda column, cell, model, iter: cell.set_property('text', note_to_name(model[iter][6])))
        self.insert_column_with_data_func(7, "Fix N#", standard_combo_renderer(model, opt_notes_ls, "/fixed_note", 7), lambda column, cell, model, iter: cell.set_property('text', note_to_name(model[iter][7])))
        self.insert_column_with_attributes(8, "Transpose", standard_combo_renderer(model, transpose_ls, "/transpose", 8), text=8)
    def drag_data_get(self, treeview, context, selection, target_id, etime):
        cursor = treeview.get_cursor()
        if cursor is not None:
            selection.set('text/plain', 8, str(cursor[0][0]))
    def drag_data_received(self, treeview, context, x, y, selection, info, etime):
        src_row = int(selection.data)
        dest_row_info = treeview.get_dest_row_at_pos(x, y)
        if dest_row_info is not None:
            dest_row = dest_row_info[0][0]
            #print src_row, dest_row, dest_row_info[1]
            cbox.do_cmd("/scene/move_layer", None, [src_row + 1, dest_row + 1])
            scene = cbox.GetThings("/scene/status", ['*layer', '*instrument', '*aux', 'name', 'title', 'transpose'], [])
            self.get_model().refresh(scene)

class SceneAuxBusesModel(gtk.ListStore):
    def __init__(self):
        gtk.ListStore.__init__(self, gobject.TYPE_STRING, gobject.TYPE_STRING)
    def refresh(self, scene_status):
        self.clear()
        for aux_name, aux_obj in scene_status.auxes.iteritems():
            # XXXKF make slots more 1st class
            slot = aux_obj.get_slot_status()
            self.append((slot.insert_preset, slot.insert_engine))

class SceneAuxBusesView(gtk.TreeView):
    def __init__(self, model):
        gtk.TreeView.__init__(self, model)
        self.insert_column_with_attributes(0, "Name", gtk.CellRendererText(), text=0)
        self.insert_column_with_attributes(1, "Engine", gtk.CellRendererText(), text=1)
    def get_current_row(self):
        if self.get_cursor()[0] is None:
            return None, None
        row = self.get_cursor()[0][0]
        return row + 1, self.get_model()[row]

class MainWindow(gtk.Window):
    def __init__(self):
        gtk.Window.__init__(self, gtk.WINDOW_TOPLEVEL)
        self.vbox = gtk.VBox(spacing = 5)
        self.add(self.vbox)
        self.create()
        set_timer(self, 30, self.update)
        self.drum_pattern_editor = None
        self.drumkit_editor = None

    def create(self):
        self.menu_bar = gtk.MenuBar()
        
        self.menu_bar.append(create_menu("_Scene", [
            ("_Load", self.load_scene),
            ("_Quit", self.quit),
        ]))
        self.menu_bar.append(create_menu("_Layer", [
            ("_Add", self.layer_add),
            ("_Remove", self.layer_remove),
        ]))
        self.menu_bar.append(create_menu("_AuxBus", [
            ("_Add", self.aux_bus_add),
            ("_Edit", self.aux_bus_edit),
            ("_Remove", self.aux_bus_remove),
        ]))
        self.menu_bar.append(create_menu("_Tools", [
            ("_Drum Kit Editor", self.tools_drumkit_editor),
            ("_Play Drum Pattern", self.tools_play_drum_pattern),
            ("_Edit Drum Pattern", self.tools_drum_pattern_editor),
        ]))
        
        self.vbox.pack_start(self.menu_bar, False, False)
        rt = cbox.GetThings("/rt/status", ['audio_channels'], [])
        scene = cbox.Document.get_scene()
        self.nb = gtk.Notebook()
        self.vbox.add(self.nb)
        self.nb.append_page(self.create_master(scene), gtk.Label("Master"))
        self.create_instrument_pages(scene.status(), rt)

    def create_master(self, scene):
        scene_status = scene.status()
        self.master_info = left_label("")
        self.timesig_info = left_label("")
        
        t = gtk.Table(3, 8)
        t.set_col_spacings(5)
        t.set_row_spacings(5)
        
        t.attach(bold_label("Scene"), 0, 1, 0, 1, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        self.scene_label = left_label(scene_status.name)
        t.attach(self.scene_label, 1, 3, 0, 1, gtk.SHRINK | gtk.FILL, gtk.SHRINK)

        self.title_label = left_label(scene_status.title)
        t.attach(bold_label("Title"), 0, 1, 1, 2, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        t.attach(self.title_label, 1, 3, 1, 2, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        
        t.attach(bold_label("Play pos"), 0, 1, 2, 3, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        t.attach(self.master_info, 1, 3, 2, 3, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
        
        t.attach(bold_label("Time sig"), 0, 1, 3, 4, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        t.attach(self.timesig_info, 1, 2, 3, 4, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        hb = gtk.HButtonBox()
        b = gtk.Button("Play")
        b.connect('clicked', lambda w: cbox.do_cmd("/master/play", None, []))
        hb.pack_start(b)
        b = gtk.Button("Stop")
        b.connect('clicked', lambda w: cbox.do_cmd("/master/stop", None, []))
        hb.pack_start(b)
        b = gtk.Button("Rewind")
        b.connect('clicked', lambda w: cbox.do_cmd("/master/seek_ppqn", None, [0]))
        hb.pack_start(b)
        t.attach(hb, 2, 3, 3, 4, gtk.EXPAND, gtk.SHRINK)
        
        t.attach(bold_label("Tempo"), 0, 1, 4, 5, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        self.tempo_adj = gtk.Adjustment(40, 40, 300, 1, 5, 0)
        self.tempo_adj.connect('value_changed', adjustment_changed_float, cbox.VarPath("/master/set_tempo"))
        t.attach(standard_hslider(self.tempo_adj), 1, 3, 4, 5, gtk.EXPAND | gtk.FILL, gtk.SHRINK)

        t.attach(bold_label("Transpose"), 0, 1, 5, 6, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        self.transpose_adj = gtk.Adjustment(scene_status.transpose, -24, 24, 1, 5, 0)
        self.transpose_adj.connect('value_changed', adjustment_changed_int, '/scene/transpose')
        t.attach(standard_align(gtk.SpinButton(self.transpose_adj), 0, 0, 0, 0), 1, 3, 5, 6, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
        
        self.layers_model = SceneLayersModel()
        self.layers_view = SceneLayersView(self.layers_model)
        t.attach(standard_vscroll_window(-1, 160, self.layers_view), 0, 3, 6, 7, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
        
        self.auxes_model = SceneAuxBusesModel()
        self.auxes_view = SceneAuxBusesView(self.auxes_model)
        t.attach(standard_vscroll_window(-1, 80, self.auxes_view), 0, 3, 7, 8, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
        
        self.layers_model.refresh(scene_status)
        self.auxes_model.refresh(scene_status)
        
        return t
        
    def quit(self, w):
        self.destroy()
        
    def load_scene(self, w):
        d = SceneDialog(self)
        response = d.run()
        try:
            if response == gtk.RESPONSE_OK:
                scene = d.get_selected_object()
                if scene[1] == 'Scene':
                    cbox.do_cmd("/scene/load", None, [scene[2][6:]])
                elif scene[1] == 'Layer':
                    cbox.do_cmd("/scene/clear", None, [])
                    cbox.do_cmd("/scene/add_layer", None, [0, scene[2][6:]])
                elif scene[1] == 'Instrument':
                    cbox.do_cmd("/scene/clear", None, [])
                    cbox.do_cmd("/scene/add_instrument", None, [0, scene[2][11:]])
                scene = cbox.GetThings("/scene/status", ['name', 'title'], [])
                self.scene_label.set_text(scene.name)
                self.title_label.set_text(scene.title)
                self.refresh_instrument_pages()
        finally:
            d.destroy()

    def layer_add(self, w):
        d = AddLayerDialog(self)
        response = d.run()
        try:
            if response == gtk.RESPONSE_OK:
                scene = d.get_selected_object()
                if scene[1] == 'Layer':
                    cbox.do_cmd("/scene/add_layer", None, [0, scene[2][6:]])
                elif scene[1] == 'Instrument':
                    cbox.do_cmd("/scene/add_instrument", None, [0, scene[2][11:]])
                self.refresh_instrument_pages()
        finally:
            d.destroy()

    def layer_remove(self, w):
        if self.layers_view.get_cursor()[0] is not None:
            pos = self.layers_view.get_cursor()[0][0]
            cbox.do_cmd("/scene/delete_layer", None, [1 + pos])
            self.refresh_instrument_pages()
            
    def aux_bus_add(self, w):
        d = fx_gui.LoadEffectDialog(self)
        response = d.run()
        try:
            cbox.do_cmd("/scene/load_aux", None, [d.get_selected_object()[0]])
            self.refresh_instrument_pages()
        finally:
            d.destroy()
    def aux_bus_remove(self, w):
        rowid, row = self.auxes_view.get_current_row()
        if rowid is None:
            return
        cbox.do_cmd("/scene/delete_aux", None, [row[0]])
        self.refresh_instrument_pages()
        
    def aux_bus_edit(self, w):
        rowid, row = self.auxes_view.get_current_row()
        if rowid is None:
            return
        wclass = fx_gui.effect_window_map[row[1]]
        popup = wclass("Aux: %s" % row[0], self, "/scene/aux/%d/engine" % rowid)
        popup.show_all()
        popup.present()
        
    def tools_drumkit_editor(self, w):
        if self.drumkit_editor is None:
            self.drumkit_editor = drumkit_editor.EditorDialog(self)
            self.drumkit_editor.connect('destroy', self.on_drumkit_editor_destroy)
            self.drumkit_editor.show_all()
        self.drumkit_editor.present()
        
    def on_drumkit_editor_destroy(self, w):
        self.drumkit_editor = None
        
    def tools_play_drum_pattern(self, w):
        d = PlayPatternDialog(self)
        response = d.run()
        try:
            if response == gtk.RESPONSE_OK:
                row = d.get_selected_object()
                if row[1] == 'Pattern':
                    song = cbox.Document().get_song()
                    song.clear()
                    track = song.add_track()
                    pat = song.load_drum_pattern(row[0])
                    length = pat.status().loop_end
                    track.add_clip(0, 0, length, pat)
                    song.set_loop(0, length)
                    song.update_playback()
                elif row[1] == 'Track':
                    song = cbox.Document().get_song()
                    song.clear()
                    track = song.add_track()
                    pat = song.load_drum_track(row[0])
                    length = pat.status().loop_end
                    track.add_clip(0, 0, length, pat)
                    song.set_loop(0, length)
                    song.update_playback()
                elif row[1] == 'Stop':
                    song = cbox.Document().get_song()
                    song.clear()
                    song.update_playback()
        finally:
            d.destroy()

    def tools_drum_pattern_editor(self, w):
        if self.drum_pattern_editor is None:
            length = drum_pattern_editor.PPQN * 4
            pat_data = cbox.Pattern.get_pattern()
            if pat_data is not None:
                pat_data, length = pat_data
            self.drum_pattern_editor = drum_pattern_editor.DrumSeqWindow(length, pat_data)
            self.drum_pattern_editor.set_title("Drum pattern editor")
            self.drum_pattern_editor.show_all()
            self.drum_pattern_editor.connect('destroy', self.on_drum_pattern_editor_destroy)
            self.drum_pattern_editor.pattern.connect('changed', self.on_drum_pattern_changed)
            self.drum_pattern_editor.pattern.changed()
        self.drum_pattern_editor.present()
        
    def on_drum_pattern_changed(self, pattern):
        data = ""
        for i in pattern.items():
            ch = i.channel - 1
            data += cbox.Pattern.serialize_event(int(i.pos), 0x90 + ch, int(i.row), int(i.vel))
            if i.len > 1:
                data += cbox.Pattern.serialize_event(int(i.pos + i.len - 1), 0x80 + ch, int(i.row), int(i.vel))
            else:
                data += cbox.Pattern.serialize_event(int(i.pos + 1), 0x80 + ch, int(i.row), int(i.vel))
        cbox.do_cmd("/play_blob", None, [buffer(data), pattern.get_length()])
        
    def on_drum_pattern_editor_destroy(self, w):
        self.drum_pattern_editor = None

    def refresh_instrument_pages(self):
        self.delete_instrument_pages()
        rt = cbox.GetThings("/rt/status", ['audio_channels'], [])
        scene_status = cbox.Document.get_scene().status()
        self.layers_model.refresh(scene_status)
        self.auxes_model.refresh(scene_status)
        self.create_instrument_pages(scene_status, rt)
        self.nb.show_all()
        self.title_label.set_text(scene_status.title)

    def create_instrument_pages(self, scene_status, rt):
        self.path_widgets = {}
        self.path_popups = {}
        self.fx_choosers = {}
        
        outputs_ls = gtk.ListStore(gobject.TYPE_STRING, gobject.TYPE_INT)
        for out in range(0, rt.audio_channels[1]/2):
            outputs_ls.append(("Out %s/%s" % (out * 2 + 1, out * 2 + 2), out))
            
        auxbus_ls = gtk.ListStore(gobject.TYPE_STRING, gobject.TYPE_STRING)
        auxbus_ls.append(("", ""))
        for bus_name in scene_status.auxes.keys():
            auxbus_ls.append(("Aux: %s" % bus_name, bus_name))
            
        for iname, iengine in scene_status.instruments.iteritems():
            ipath = "/scene/instr/%s" % iname
            idata = cbox.GetThings(ipath + "/status", ['outputs', 'aux_offset'], [])
            #attribs = cbox.GetThings("/scene/instr_info", ['engine', 'name'], [i])
            #markup += '<b>Instrument %d:</b> engine %s, name %s\n' % (i, attribs.engine, attribs.name)
            b = gtk.VBox(spacing = 5)
            b.set_border_width(5)
            b.pack_start(gtk.Label("Engine: %s" % iengine), False, False)
            b.pack_start(gtk.HSeparator(), False, False)
            t = gtk.Table(1 + idata.outputs, 7)
            t.set_col_spacings(5)
            t.attach(bold_label("Instr. output", 0.5), 0, 1, 0, 1, gtk.SHRINK, gtk.SHRINK)
            t.attach(bold_label("Send to", 0.5), 1, 2, 0, 1, gtk.SHRINK, gtk.SHRINK)
            t.attach(bold_label("Gain [dB]", 0.5), 2, 3, 0, 1, 0, gtk.SHRINK)
            t.attach(bold_label("Effect", 0.5), 3, 4, 0, 1, 0, gtk.SHRINK)
            t.attach(bold_label("Preset", 0.5), 4, 7, 0, 1, 0, gtk.SHRINK)
            b.pack_start(t, False, False)
            
            y = 1
            for o in range(1, idata.outputs + 1):
                is_aux = o >= idata.aux_offset
                if not is_aux:
                    opath = "%s/output/%s" % (ipath, o)
                    output_name = "Out %s" % o
                else:
                    opath = "%s/aux/%s" % (ipath, o - idata.aux_offset + 1)
                    output_name = "Aux %s" % (o - idata.aux_offset + 1)
                odata = cbox.GetThings(opath + "/status", ['gain', 'output', 'bus', 'insert_engine', 'insert_preset', 'bypass'], [])
                engine = odata.insert_engine
                preset = odata.insert_preset
                bypass = odata.bypass
                
                t.attach(gtk.Label(output_name), 0, 1, y, y + 1, gtk.SHRINK, gtk.SHRINK)
                
                if not is_aux:
                    cb = standard_combo(outputs_ls, odata.output - 1)
                    cb.connect('changed', combo_value_changed, cbox.VarPath(opath + '/output'), 1)
                else:
                    cb = standard_combo(auxbus_ls, ls_index(auxbus_ls, odata.bus, 1))
                    cb.connect('changed', combo_value_changed_use_column, cbox.VarPath(opath + '/bus'), 1)
                t.attach(cb, 1, 2, y, y + 1, gtk.SHRINK, gtk.SHRINK)
                                    
                adj = gtk.Adjustment(odata.gain, -96, 24, 1, 6, 0)
                adj.connect('value_changed', adjustment_changed_float, cbox.VarPath(opath + '/gain'))
                t.attach(standard_hslider(adj), 2, 3, y, y + 1, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
                
                chooser = fx_gui.InsertEffectChooser(opath, "%s: %s" % (iname, output_name), engine, preset, bypass, self)
                self.fx_choosers[opath] = chooser
                t.attach(chooser.fx_engine, 3, 4, y, y + 1, 0, gtk.SHRINK)
                t.attach(chooser.fx_preset, 4, 5, y, y + 1, 0, gtk.SHRINK)
                t.attach(chooser.fx_edit, 5, 6, y, y + 1, 0, gtk.SHRINK)
                t.attach(chooser.fx_bypass, 6, 7, y, y + 1, 0, gtk.SHRINK)
                y += 1
            if iengine in instr_gui.instrument_window_map:
                b.pack_start(gtk.HSeparator(), False, False)
                b.pack_start(instr_gui.instrument_window_map[iengine](iname, "/scene/instr/%s/engine" % iname), True, True)
            self.nb.append_page(b, gtk.Label(iname))
        self.update()
        
    def delete_instrument_pages(self):
        while self.nb.get_n_pages() > 1:
            self.nb.remove_page(self.nb.get_n_pages() - 1)
            
    def update(self):
        cbox.do_cmd("/on_idle", None, [])
        master = cbox.GetThings("/master/status", ['pos', 'pos_ppqn', 'tempo', 'timesig'], [])
        if master.tempo is not None:
            self.master_info.set_markup('%s (%s)' % (master.pos, master.pos_ppqn))
            self.timesig_info.set_markup("%s/%s" % tuple(master.timesig))
            self.tempo_adj.set_value(master.tempo)
        return True

def do_quit(window):
    gtk.main_quit()

w = MainWindow()
w.set_title("My UI")
w.show_all()
w.connect('destroy', do_quit)

gtk.main()

