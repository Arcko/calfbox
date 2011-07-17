import cbox
import glob
import os
from gui_tools import *

#sample_dir = "/media/resources/samples/dooleydrums/"
sample_dir = cbox.Config.get("init", "sample_dir")

class SampleFilesModel(gtk.ListStore):
    def __init__(self):
        gtk.ListStore.__init__(self, gobject.TYPE_STRING, gobject.TYPE_STRING)
        
    def refresh(self):
        self.clear()
        self.append(("","<empty>"))
        for f in glob.glob("%s/*" % sample_dir):
            print f
            if not f.lower().endswith(".wav"):
                continue
            self.append((f,os.path.basename(f)))

class KeyModelPath(object):
    def __init__(self, controller, var = None):
        self.controller = controller
        self.var = var
        self.args = []
    def plus(self, var):
        if self.var is not None:
            print "Warning: key model plus used twice with %s and %s" % (self.var, var)
        return KeyModelPath(self.controller, var)
    def set(self, value):
        model = self.controller.get_current_key_model()
        oldval = getattr(model, self.var)
        setattr(model, self.var, value)
        if value != oldval:
            print "%s: set %s to %s" % (self.controller, self.var, value)
            self.controller.update_kit()

class SFZRegion(object):
    volume = 0
    pan = 0
    ampeg_attack = 0.001
    ampeg_hold = 0.001
    ampeg_decay = 0.001
    ampeg_sustain = 100
    ampeg_release = 0.1

class KeyModel(object):
    def __init__(self, key, sample, filename):
        self.key = key
        self.sample = sample
        self.filename = filename
        self.mode = "one_shot"
        for key in dir(SFZRegion):
            if not key.startswith("__"):
                setattr(self, key, getattr(SFZRegion, key))
    def set_sample(self, sample, filename):
        self.sample = sample
        self.filename = filename
    def to_sfz(self):
        if self.filename == '':
            return ""
        s = "<region> key=%d sample=%s loop_mode=%s" % (self.key, self.filename, self.mode)
        s += "".join([" %s=%s" % (key, getattr(self, key)) for key in dir(SFZRegion) if not key.startswith("__")])
        s += "\n"
        return s
    def to_markup(self):
        return "<small>%s</small>" % self.sample

class BankModel(object):
    def __init__(self):
        self.keys = {}
    def __getitem__(self, key):
        if key in self.keys:
            return self.keys[key]
        return None
    def __setitem__(self, key, value):
        if value.filename == "":
            if key in self.keys:
                del self.keys[key]
        else:
            self.keys[key] = value
    def to_sfz(self):
        s = ""
        for key in self.keys:
            s += self.keys[key].to_sfz()
        return s

class PadEditor(gtk.VBox):
    def __init__(self, controller, bank_model):
        gtk.VBox.__init__(self)
        self.table = gtk.Table(len(self.fields) + 1, 2)
        self.table.set_size_request(240, -1)
        self.controller = controller
        self.bank_model = bank_model
        self.name_widget = gtk.Label()
        self.table.attach(self.name_widget, 0, 2, 0, 1)
        self.refreshers = []
        for i in range(len(self.fields)):
            self.refreshers.append(self.fields[i].add_row(self.table, i + 1, KeyModelPath(controller), None))
            #self.table.attach(left_label(self.fields[i].label), 0, 1, i + 1, i + 2)
        self.pack_start(self.table, False, False)
        
    def refresh(self):
        data = self.controller.get_current_key_model()
        if data is None:
            self.name_widget.set_text("")
        else:
            self.name_widget.set_text(data.sample)
        for r in self.refreshers:
            r(data)

    fields = [
        SliderRow("Volume", "volume", -100, 0),
        SliderRow("Pan", "pan", -100, 100),
        MappedSliderRow("Amp Attack", "ampeg_attack", env_mapper),
        MappedSliderRow("Amp Hold", "ampeg_hold", env_mapper),
        MappedSliderRow("Amp Decay", "ampeg_decay", env_mapper),
        SliderRow("Amp Sustain", "ampeg_sustain", 0, 100),
        MappedSliderRow("Amp Release", "ampeg_release", env_mapper),
    ]
    
class PadButton(gtk.RadioButton):
    def __init__(self, controller, model, key):
        gtk.RadioButton.__init__(self, use_underline = False)
        self.set_mode(False)
        self.controller = controller
        self.model = model
        self.key = key
        self.set_size_request(100, 100)
        self.update_label()
        self.drag_dest_set(gtk.DEST_DEFAULT_ALL, [("text/plain", 0, 1)], gtk.gdk.ACTION_COPY)
        self.connect('drag_data_received', self.drag_data_received)
        self.connect('toggled', lambda widget: widget.controller.on_pad_selected(widget) if widget.get_active() else None)
    def drag_data_received(self, widget, context, x, y, selection, info, etime):
        sample, filename = selection.data.split("|")
        km = self.model[self.key]
        if km is not None:
            km.set_sample(sample, filename)
        else:
            km = KeyModel(self.key, sample, filename)
            self.model[self.key] = km
        self.update_label()
        self.controller.on_sample_dragged(self)
    def update_label(self):
        data = self.model[self.key]
        if data == None:
            self.set_label("-")
        else:
            self.get_child().set_markup(data.to_markup())
            self.get_child().set_line_wrap(True)

class PadTable(gtk.Table):
    def __init__(self, controller, model, rows, columns):
        gtk.Table.__init__(self, rows, columns, True)
        
        self.keys = {}
        group = None
        for r in range(0, rows):
            for c in range(0, columns):
                key = 36 + (rows - r - 1) * columns + c
                b = PadButton(controller, model, key)
                b.set_group(group)
                a = gtk.Alignment(0.5, 0.5)
                a.add(b)
                group = b
                self.attach(a, c, c + 1, r, r + 1)
                self.keys[key] = b

class FileView(gtk.TreeView):
    def __init__(self):
        self.files_model = SampleFilesModel()
        self.files_model.refresh()
        gtk.TreeView.__init__(self, self.files_model)
        self.insert_column_with_attributes(0, "Name", gtk.CellRendererText(), text=1)
        self.set_cursor((0,))
        self.enable_model_drag_source(gtk.gdk.BUTTON1_MASK, [("text/plain", 0, 1)], gtk.gdk.ACTION_COPY)
        self.connect('cursor-changed', self.cursor_changed)
        self.connect('drag-data-get', self.drag_data_get)
        
    def cursor_changed(self, w):
        c = self.get_cursor()
        if c[0] is not None:
            fn = self.files_model[c[0][0]][0]
            if fn != "":
                cbox.do_cmd("/instr/_preview_sample/engine/load", None, [fn, -1])
                cbox.do_cmd("/instr/_preview_sample/engine/play", None, [])
            else:
                cbox.do_cmd("/instr/_preview_sample/engine/unload", None, [])
            
    def drag_data_get(self, treeview, context, selection, target_id, etime):
        cursor = treeview.get_cursor()
        if cursor is not None:
            c = cursor[0][0]
            fr = self.files_model[c]
            selection.set('text/plain', 8, str(fr[1]+"|"+fr[0]))
        
class EditorDialog(gtk.Dialog):
    def __init__(self, parent):
        gtk.Dialog.__init__(self, "Drum kit editor", parent, gtk.DIALOG_MODAL, 
            (gtk.STOCK_OK, gtk.RESPONSE_OK))
        self.set_default_response(gtk.RESPONSE_OK)
        self.hbox = gtk.HBox()
        
        self.current_pad = None
        self.bank_model = BankModel()
        self.tree = FileView()
        self.pad_editor = PadEditor(self, self.bank_model)
        
        sw = gtk.ScrolledWindow()
        sw.add_with_viewport(self.tree)
        self.hbox.pack_start(sw, True, True)
        sw.set_size_request(240, -1)
        self.pads = PadTable(self, self.bank_model, 4, 4)
        self.hbox.pack_start(self.pads, True, True)
        self.hbox.pack_start(self.pad_editor, True, True)
        self.vbox.pack_start(self.hbox)
        self.vbox.show_all()
        widget = self.pads.keys[36]
        widget.set_active(True)
        
        self.update_kit()

    def update_kit(self):
        cbox.do_cmd("/instr/_preview_kit/engine/load_patch_from_string", None, [0, "", self.bank_model.to_sfz(), "Preview"])
        
    def on_sample_dragged(self, widget):
        self.update_kit()
        print widget, self.current_pad
        if widget == self.current_pad:
            self.pad_editor.refresh()
        
    def on_pad_selected(self, widget):
        self.current_pad = widget
        print widget.key
        self.pad_editor.refresh()
    
    def get_current_key_model(self):
        if self.current_pad is None or self.current_pad.key is None:
            return None
        return self.bank_model[self.current_pad.key]
