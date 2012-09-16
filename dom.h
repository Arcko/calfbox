/*
Calf Box, an open source musical instrument.
Copyright (C) 2012 Krzysztof Foltman

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef CBOX_DOM_H
#define CBOX_DOM_H

#include <glib.h>
#include <uuid/uuid.h>

struct cbox_command_target;
struct cbox_osc_command;
struct cbox_objhdr;
struct cbox_document;
struct GList;

struct cbox_uuid
{
    uuid_t uuid;
};

struct cbox_class
{
    struct cbox_class *parent;
    const char *name;
    void (*destroyfunc)(struct cbox_objhdr *objhdr);
    struct cbox_command_target *(*getcmdtargetfunc)(struct cbox_objhdr *objhdr);
};

extern struct cbox_class *cbox_class_find_by_name(const char *name);
extern void cbox_class_register(struct cbox_class *class_ptr);

struct cbox_objhdr
{
    struct cbox_class *class_ptr;
    struct cbox_document *owner;
    void *link_in_document;
    struct cbox_uuid instance_uuid;
};

inline int cbox_class_is_a(struct cbox_class *c1, struct cbox_class *c2)
{
    while(c1 != c2 && c1->parent)
        c1 = c1->parent;
    return c1 == c2;
}

extern void cbox_object_register_instance(struct cbox_document *doc, struct cbox_objhdr *obj);
extern struct cbox_command_target *cbox_object_get_cmd_target(struct cbox_objhdr *hdr_ptr);
extern gboolean cbox_object_default_process_cmd(struct cbox_command_target *ct, struct cbox_command_target *fb, struct cbox_osc_command *cmd, GError **error);
extern gboolean cbox_object_default_status(struct cbox_objhdr *objhdr, struct cbox_command_target *fb, GError **error);

extern void cbox_object_destroy(struct cbox_objhdr *hdr_ptr);

extern struct cbox_document *cbox_document_new();
extern void cbox_document_dump(struct cbox_document *);
extern struct cbox_command_target *cbox_document_get_cmd_target(struct cbox_document *);
extern struct cbox_objhdr *cbox_document_get_service(struct cbox_document *doc, const char *name);
extern void cbox_document_set_service(struct cbox_document *doc, const char *name, struct cbox_objhdr *hdr_ptr);
extern struct cbox_objhdr *cbox_document_get_object_by_uuid(struct cbox_document *doc, const struct cbox_uuid *uuid);
extern void cbox_document_destroy(struct cbox_document *);

extern void cbox_dom_init();
extern void cbox_dom_close();

// must be the first field in the object-compatible struct
#define CBOX_OBJECT_HEADER() \
    struct cbox_objhdr _obj_hdr;

#define CBOX_EXTERN_CLASS(class) \
    extern struct cbox_class CBOX_CLASS_##class;

#define CBOX_GET_DOCUMENT(obj) \
    ((obj)->_obj_hdr.owner)

#define CBOX_DELETE(obj) \
    (obj) && (cbox_object_destroy(&(obj)->_obj_hdr), 1)

#define CBOX_OBJECT_HEADER_INIT(self, class, document) \
    do { \
        (self)->_obj_hdr.class_ptr = &CBOX_CLASS_##class; \
        (self)->_obj_hdr.owner = (document); \
        (self)->_obj_hdr.link_in_document = NULL; \
        uuid_generate((self)->_obj_hdr.instance_uuid.uuid); \
    } while(0)
    
#define CBOX_OBJECT_REGISTER(self) \
    (cbox_object_register_instance((self)->_obj_hdr.owner, &(self)->_obj_hdr))

#define CBOX_OBJECT_DEFAULT_STATUS(self, fb, error) \
    (cbox_object_default_status(&(self)->_obj_hdr, (fb), (error)))

#define CBOX_CLASS_DEFINITION_ROOT(class) \
    static void class##_destroyfunc(struct cbox_objhdr *hdr_ptr); \
    static struct cbox_command_target *class##_getcmdtarget(struct cbox_objhdr *hdr) { \
        return &(((struct class *)hdr)->cmd_target);\
    }; \
    struct cbox_class CBOX_CLASS_##class = { \
        .parent = NULL, \
        .name = #class, \
        .destroyfunc = class##_destroyfunc, \
        .getcmdtargetfunc = class##_getcmdtarget \
    }; \
    
#define CBOX_RETURN_OBJECT(result) \
    return &(result)->_obj_hdr

#endif
