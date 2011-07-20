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

in_channels_ls = gtk.ListStore(gobject.TYPE_INT, gobject.TYPE_STRING)
in_channels_ls.append((0, "All"))
out_channels_ls = gtk.ListStore(gobject.TYPE_INT, gobject.TYPE_STRING)
out_channels_ls.append((0, "Same"))
for i in range(1, 17):
    in_channels_ls.append((i, str(i)))
    out_channels_ls.append((i, str(i)))

class MainWindow(gtk.Window):
    def __init__(self):
        gtk.Window.__init__(self, gtk.WINDOW_TOPLEVEL)
        self.vbox = gtk.VBox(spacing = 5)
        self.add(self.vbox)
        self.create()
        set_timer(self, 30, self.update)

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
        self.menu_bar.append(create_menu("_Tools", [
            ("_Drum Kit Editor", self.tools_drumkit_editor),
        ]))
        
        self.vbox.pack_start(self.menu_bar, False, False)
        rt = cbox.GetThings("/rt/status", ['audio_channels'], [])
        scene = cbox.GetThings("/scene/status", ['*layer', '*instrument', '*aux', 'name', 'title', 'transpose'], [])        
        self.nb = gtk.Notebook()
        self.vbox.add(self.nb)
        self.nb.append_page(self.create_master(scene), gtk.Label("Master"))
        self.create_instrument_pages(scene, rt)

    def create_master(self, scene):
        self.scene_list = gtk.ListStore(gobject.TYPE_STRING, gobject.TYPE_STRING)
                
        self.master_info = left_label("")
        self.timesig_info = left_label("")
        
        t = gtk.Table(2, 6)
        t.set_col_spacings(5)
        t.set_row_spacings(5)
        
        t.attach(bold_label("Scene"), 0, 1, 0, 1, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        self.scene_label = left_label(scene.name)
        t.attach(self.scene_label, 1, 2, 0, 1, gtk.SHRINK | gtk.FILL, gtk.SHRINK)

        self.title_label = left_label(scene.title)
        t.attach(bold_label("Title"), 0, 1, 1, 2, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        t.attach(self.title_label, 1, 2, 1, 2, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        
        t.attach(bold_label("Play pos"), 0, 1, 2, 3, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        t.attach(self.master_info, 1, 2, 2, 3, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        
        t.attach(bold_label("Time sig"), 0, 1, 3, 4, gtk.SHRINK, gtk.SHRINK)
        t.attach(self.timesig_info, 1, 2, 3, 4, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        
        t.attach(bold_label("Tempo"), 0, 1, 4, 5, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        self.tempo_adj = gtk.Adjustment(40, 40, 300, 1, 5, 0)
        self.tempo_adj.connect('value_changed', adjustment_changed_float, cbox.VarPath("/master/set_tempo"))
        t.attach(standard_hslider(self.tempo_adj), 1, 2, 4, 5, gtk.EXPAND | gtk.FILL, gtk.SHRINK)

        t.attach(bold_label("Transpose"), 0, 1, 5, 6, gtk.SHRINK | gtk.FILL, gtk.SHRINK)
        self.transpose_adj = gtk.Adjustment(scene.transpose, -24, 24, 1, 5, 0)
        self.transpose_adj.connect('value_changed', adjustment_changed_int, '/scene/transpose')
        t.attach(standard_align(gtk.SpinButton(self.transpose_adj), 0, 0, 0, 0), 1, 2, 5, 6, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
        
        self.layers_model = gtk.ListStore(gobject.TYPE_STRING, gobject.TYPE_BOOLEAN, 
            gobject.TYPE_INT, gobject.TYPE_INT, gobject.TYPE_BOOLEAN,
            gobject.TYPE_INT, gobject.TYPE_INT, gobject.TYPE_INT, gobject.TYPE_INT)    
        self.layers_tree = gtk.TreeView(self.layers_model)
        self.layers_tree.enable_model_drag_source(gtk.gdk.BUTTON1_MASK, [("text/plain", 0, 1)], gtk.gdk.ACTION_MOVE)
        self.layers_tree.enable_model_drag_dest([("text/plain", gtk.TARGET_SAME_APP | gtk.TARGET_SAME_WIDGET, 1)], gtk.gdk.ACTION_MOVE)
        self.layers_tree.connect('drag_data_get', self.drag_data_get)
        self.layers_tree.connect('drag_data_received', self.drag_data_received)
        self.layers_tree.insert_column_with_attributes(0, "On?", standard_toggle_renderer(self.layers_model, "/scene/layer/%d/enable", 1), active=1)
        self.layers_tree.insert_column_with_attributes(1, "Name", gtk.CellRendererText(), text=0)
        self.layers_tree.insert_column_with_data_func(2, "In Ch#", standard_combo_renderer(self.layers_model, in_channels_ls, "/scene/layer/%d/in_channel", 2), lambda column, cell, model, iter: cell.set_property('text', "%s" % model[iter][2] if model[iter][2] != 0 else 'All'))
        self.layers_tree.insert_column_with_data_func(3, "Out Ch#", standard_combo_renderer(self.layers_model, out_channels_ls, "/scene/layer/%d/out_channel", 3), lambda column, cell, model, iter: cell.set_property('text', "%s" % model[iter][3] if model[iter][3] != 0 else 'Same'))
        self.layers_tree.insert_column_with_attributes(4, "Eat?", standard_toggle_renderer(self.layers_model, "/scene/layer/%d/consume", 4), active=4)
        self.layers_tree.insert_column_with_attributes(5, "Lo N#", gtk.CellRendererText(), text=5)
        self.layers_tree.insert_column_with_attributes(6, "Hi N#", gtk.CellRendererText(), text=6)
        self.layers_tree.insert_column_with_attributes(7, "Fix N#", gtk.CellRendererText(), text=7)
        self.layers_tree.insert_column_with_attributes(8, "Transpose", gtk.CellRendererText(), text=8)
        t.attach(self.layers_tree, 0, 2, 6, 7, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
        self.refresh_layer_list(scene)        
        return t
        
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
            self.refresh_layer_list(scene)

    def refresh_layer_list(self, scene):
        self.layers_model.clear()
        for l in scene.layer:
            layer = cbox.GetThings("/scene/layer/%d/status" % l, ["enable", "instrument_name", "in_channel", "out_channel", "consume", "low_note", "high_note", "fixed_note", "transpose"], [])
            self.layers_model.append((layer.instrument_name, layer.enable != 0, layer.in_channel, layer.out_channel, layer.consume != 0, layer.low_note, layer.high_note, layer.fixed_note, layer.transpose))

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
                    cbox.do_cmd("/scene/new", None, [])
                    cbox.do_cmd("/scene/add_layer", None, [0, scene[2][6:]])
                elif scene[1] == 'Instrument':
                    cbox.do_cmd("/scene/new", None, [])
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
        if self.layers_tree.get_cursor()[0] is not None:
            pos = self.layers_tree.get_cursor()[0][0]
            cbox.do_cmd("/scene/delete_layer", None, [1 + pos])
            self.refresh_instrument_pages()
            
    def tools_drumkit_editor(self, w):
        dlg = drumkit_editor.EditorDialog(self)
        dlg.run()
        dlg.destroy()

    def refresh_instrument_pages(self):
        self.delete_instrument_pages()
        rt = cbox.GetThings("/rt/status", ['audio_channels'], [])
        scene = cbox.GetThings("/scene/status", ['*layer', '*instrument', '*aux', 'name', 'title', 'transpose'], [])
        self.refresh_layer_list(scene)
        self.create_instrument_pages(scene, rt)
        self.nb.show_all()
        self.title_label.set_text(scene.title)

    def create_instrument_pages(self, scene, rt):
        self.path_widgets = {}
        self.path_popups = {}
        self.fx_choosers = {}
        
        outputs_ls = gtk.ListStore(gobject.TYPE_STRING, gobject.TYPE_INT)
        for out in range(0, rt.audio_channels[1]/2):
            outputs_ls.append(("Out %s/%s" % (out * 2 + 1, out * 2 + 2), out))
            
        auxbus_ls = gtk.ListStore(gobject.TYPE_STRING, gobject.TYPE_STRING)
        auxbus_ls.append(("", ""))
        for bus in range(len(scene.aux)):
            auxbus_ls.append(("Aux: %s" % scene.aux[bus][1], scene.aux[bus][1]))
            
        for i in scene.instrument:
            ipath = "/instr/%s" % i[0]
            idata = cbox.GetThings(ipath + "/status", ['outputs', 'aux_offset'], [])
            #attribs = cbox.GetThings("/scene/instr_info", ['engine', 'name'], [i])
            #markup += '<b>Instrument %d:</b> engine %s, name %s\n' % (i, attribs.engine, attribs.name)
            b = gtk.VBox(spacing = 5)
            b.set_border_width(5)
            b.pack_start(gtk.Label("Engine: %s" % i[1]), False, False)
            b.pack_start(gtk.HSeparator(), False, False)
            t = gtk.Table(1 + idata.outputs, 5)
            t.set_col_spacings(5)
            t.attach(bold_label("Instr. output", 0.5), 0, 1, 0, 1, gtk.SHRINK, gtk.SHRINK)
            t.attach(bold_label("Send to", 0.5), 1, 2, 0, 1, gtk.SHRINK, gtk.SHRINK)
            t.attach(bold_label("Gain [dB]", 0.5), 2, 3, 0, 1, 0, gtk.SHRINK)
            t.attach(bold_label("Effect", 0.5), 3, 4, 0, 1, 0, gtk.SHRINK)
            t.attach(bold_label("Preset", 0.5), 4, 6, 0, 1, 0, gtk.SHRINK)
            b.pack_start(t, False, False)
            
            y = 1
            for o in range(1, idata.outputs + 1):
                if o < idata.aux_offset:
                    opath = "%s/output/%s" % (ipath, o)
                else:
                    opath = "%s/aux/%s" % (ipath, o - idata.aux_offset + 1)
                odata = cbox.GetThings(opath + "/status", ['gain', 'output', 'bus', 'insert_engine', 'insert_preset'], [])
                engine = odata.insert_engine
                preset = odata.insert_preset
                
                is_aux = 2 * (o - 1) >= idata.aux_offset
                
                if not is_aux:
                    output_name = "Out %s" % o
                else:
                    output_name = "Aux %s" % (o - idata.aux_offset / 2)
                t.attach(gtk.Label(output_name), 0, 1, y, y + 1, gtk.SHRINK, gtk.SHRINK)
                
                if not is_aux:
                    cb = standard_combo(outputs_ls, odata.output - 1)
                    cb.connect('changed', combo_value_changed, opath + '/output', 1)
                else:
                    cb = standard_combo(auxbus_ls, ls_index(auxbus_ls, odata.bus, 1))
                    cb.connect('changed', combo_value_changed_use_column, opath + '/bus', 1)
                t.attach(cb, 1, 2, y, y + 1, gtk.SHRINK, gtk.SHRINK)
                                    
                adj = gtk.Adjustment(odata.gain, -96, 24, 1, 6, 0)
                adj.connect('value_changed', adjustment_changed_float, cbox.VarPath(opath + '/gain'))
                t.attach(standard_hslider(adj), 2, 3, y, y + 1, gtk.EXPAND | gtk.FILL, gtk.SHRINK)
                
                chooser = fx_gui.InsertEffectChooser(opath, "%s: %s" % (i[0], output_name), engine, preset, self)
                self.fx_choosers[opath] = chooser
                t.attach(chooser.fx_engine, 3, 4, y, y + 1, 0, gtk.SHRINK)
                t.attach(chooser.fx_preset, 4, 5, y, y + 1, 0, gtk.SHRINK)
                t.attach(chooser.fx_edit, 5, 6, y, y + 1, 0, gtk.SHRINK)
                y += 1
            if i[1] in instr_gui.instrument_window_map:
                b.pack_start(gtk.HSeparator(), False, False)
                b.pack_start(instr_gui.instrument_window_map[i[1]](i[0], "/instr/%s/engine" % i[0]), True, True)
            self.nb.append_page(b, gtk.Label(i[0]))
        self.update()
        
    def delete_instrument_pages(self):
        while self.nb.get_n_pages() > 1:
            self.nb.remove_page(self.nb.get_n_pages() - 1)
            
    def update(self):
        cbox.do_cmd("/on_idle", None, [])
        master = cbox.GetThings("/master/status", ['pos', 'tempo', 'timesig'], [])
        self.master_info.set_markup('%s' % master.pos)
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

