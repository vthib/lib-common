/***************************************************************************/
/*                                                                         */
/* Copyright 2019 INTERSEC SA                                              */
/*                                                                         */
/* Licensed under the Apache License, Version 2.0 (the "License");         */
/* you may not use this file except in compliance with the License.        */
/* You may obtain a copy of the License at                                 */
/*                                                                         */
/*     http://www.apache.org/licenses/LICENSE-2.0                          */
/*                                                                         */
/* Unless required by applicable law or agreed to in writing, software     */
/* distributed under the License is distributed on an "AS IS" BASIS,       */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*/
/* See the License for the specific language governing permissions and     */
/* limitations under the License.                                          */
/*                                                                         */
/***************************************************************************/

#include "iopc.h"

/* {{{ Helpers */

static const char *reserved_names_g[] = {
    /* TODO: incomplete list */
    "type", "module",
};

#define RO_WARN \
    "/***** THIS FILE IS AUTOGENERATED DO NOT MODIFY DIRECTLY ! *****/\n"

static qv_t(str) pp_g;

static const char *pp_under(iopc_path_t *path)
{
    SB_1k(buf);
    char *res;

    tab_for_each_entry(bit, &path->bits) {
        sb_addf(&buf, "%s__", bit);
    }
    sb_shrink(&buf, 2);
    qv_append(&pp_g, res = sb_detach(&buf, NULL));
    return res;
}

static const char *pp_path(iopc_path_t *path)
{
    SB_1k(buf);
    char *res;

    tab_for_each_entry(bit, &path->bits) {
        sb_addf(&buf, "%s/", bit);
    }
    sb_shrink(&buf, 1);
    qv_append(&pp_g, res = sb_detach(&buf, NULL));
    return res;
}

static bool is_name_reserved(const char *field_name)
{
    carray_for_each_entry(name, reserved_names_g) {
        if (strequal(field_name, name)) {
            return true;
        }
    }

    return false;
}

static lstr_t t_field_name_to_camelcase(const char *s)
{
    lstr_t res = t_lstr_fmt("%s", s);

    if (res.len > 0) {
        res.v[0] = toupper(res.v[0]);
    }

    return res;
}

/* }}} */
/* Import {{{ */

static void iopc_dump_import(sb_t *buf, const iopc_pkg_t *dep,
                             qh_t(cstr) *imported)
{
    const char *import_name = pp_under(dep->name);

    if (qh_put(cstr, imported, import_name, 0) & QHASH_COLLISION) {
        return;
    }

    sb_addf(buf, "import * as %s from \"iop/%s.iop\";\n",
            pp_under(dep->name), pp_path(dep->name));
}

static void iopc_dump_imports(sb_t *buf, iopc_pkg_t *pkg)
{
    qh_t(cstr) imported;
    qv_t(iopc_pkg) t_deps;
    qv_t(iopc_pkg) t_weak_deps;
    qv_t(iopc_pkg) i_deps;

    qh_init(cstr, &imported);
    qv_inita(&t_deps, 1024);
    qv_inita(&t_weak_deps, 1024);
    qv_inita(&i_deps, 1024);

    iopc_get_depends(pkg, &t_deps, &t_weak_deps, &i_deps, 0);

    tab_for_each_entry(dep, &t_deps) {
        iopc_dump_import(buf, dep, &imported);
    }
    tab_for_each_entry(dep, &t_weak_deps) {
        iopc_dump_import(buf, dep, &imported);
    }
    tab_for_each_entry(dep, &i_deps) {
        iopc_dump_import(buf, dep, &imported);
    }

    qv_wipe(&t_deps);
    qv_wipe(&t_weak_deps);
    qv_wipe(&i_deps);
    qh_wipe(cstr, &imported);
}

/* }}} */
/* Struct/Enum/Union {{{ */

static void iopc_dump_package_member(sb_t *buf, const iopc_pkg_t *pkg,
                                     const iopc_pkg_t *member_pkg,
                                     iopc_path_t *member_path,
                                     const char *member_name)
{
    if (pkg != member_pkg) {
        assert (member_path->bits.len);
        sb_adds(buf, pp_under(member_path));
        sb_addc(buf, '.');
    }
    sb_adds(buf, member_name);
}

static void iopc_dump_enum(sb_t *buf, const char *indent,
                           const iopc_pkg_t *pkg, const iopc_enum_t *en)
{
#if 0
    bool is_strict = false;

    tab_for_each_entry(attr, &en->attrs) {
        if (attr->desc->id == IOPC_ATTR_STRICT) {
            is_strict = true;
            break;
        }
    }

    /* FIXME: handle is_strict */
#endif

    sb_addf(buf, "\n%s#[derive(PartialEq, Eq, Clone, "
            "Serialize_repr, Deserialize_repr)]", indent);
    sb_addf(buf, "\n%s#[repr(i32)]", indent);
    sb_addf(buf, "\n%spub enum %s {", indent, en->name);
    tab_for_each_entry(field, &en->values) {
        sb_addf(buf, "\n%s    %s = %d,", indent, field->name, field->value);
    }
    sb_addf(buf, "\n%s}\n", indent);

    /* FIXME: Can this test even be false? */
    if (en->values.len > 0) {
        /* In order to have a default initializer for structs, we need one for
         * enums as well (as they can be used as struct fields). */
        sb_addf(buf, "%simpl Default for %s {\n", indent, en->name);
        sb_addf(buf, "%s    fn default() -> Self {\n", indent);
        sb_addf(buf, "%s        %s::%s\n", indent, en->name,
                en->values.tab[0]->name);
        sb_addf(buf, "%s    }\n", indent);
        sb_addf(buf, "%s}\n", indent);
    }
}

static void iopc_dump_enums(sb_t *buf, const iopc_pkg_t *pkg)
{
    tab_for_each_entry(en, &pkg->enums) {
        iopc_dump_enum(buf, "", pkg, en);
        sb_addc(buf, '\n');
    }
}

static void iopc_dump_field_basetype(sb_t *buf, const iopc_pkg_t *pkg,
                                     const iopc_field_t *field)
{
    switch (field->kind) {
      case IOP_T_I8: sb_adds(buf, "i8"); break;
      case IOP_T_U8: sb_adds(buf, "u8"); break;
      case IOP_T_I16: sb_adds(buf, "i16"); break;
      case IOP_T_U16: sb_adds(buf, "u16"); break;
      case IOP_T_I32: sb_adds(buf, "i32"); break;
      case IOP_T_U32: sb_adds(buf, "u32"); break;
      case IOP_T_I64: sb_adds(buf, "i64"); break;
      case IOP_T_U64: sb_adds(buf, "u64"); break;
      case IOP_T_BOOL: sb_adds(buf, "bool"); break;
      case IOP_T_DOUBLE: sb_adds(buf, "f64"); break;
      case IOP_T_VOID: sb_adds(buf, "()"); break;

      case IOP_T_STRING: case IOP_T_XML:
      case IOP_T_DATA:
        sb_adds(buf, "String");
        break;

      case IOP_T_STRUCT: {
        bool is_class = iopc_is_class(field->struct_def->type);

        if (is_class) {
            /* TODO: handle classes. This is a placeholder. */
            sb_adds(buf, "Box<dyn ");
        } else
        if (field->is_ref) {
            sb_adds(buf, "Box<");
        }

        iopc_dump_package_member(buf, pkg, field->type_pkg, field->type_path,
                                 field->type_name);
        if (is_class || field->is_ref) {
            sb_adds(buf, ">");
        }
      } break;

      case IOP_T_UNION: case IOP_T_ENUM:
        iopc_dump_package_member(buf, pkg, field->type_pkg, field->type_path,
                                 field->type_name);
        break;
    }
}

typedef enum iopc_rust_dump_flags_t {
    DEFVAL_AS_OPT = 1 << 0,
    ENUM_STYLE = 1 << 1,
} iopc_rust_dump_flags_t;

static void iopc_dump_field_type(sb_t *buf, const iopc_pkg_t *pkg,
                                 const iopc_field_t *field,
                                 iopc_rust_dump_flags_t flags)
{
    bool close_bracket = false;

    switch (field->repeat) {
      case IOP_R_REPEATED:
        sb_adds(buf, "Vec<");
        close_bracket = true;
        break;
      case IOP_R_DEFVAL:
        if (!(flags & DEFVAL_AS_OPT)) {
            break;
        }
        /* FALLTHROUGH */
      case IOP_R_OPTIONAL:
        sb_adds(buf, "Option<");
        close_bracket = true;
        break;
      default:
        break;
    }

    iopc_dump_field_basetype(buf, pkg, field);

    if (close_bracket) {
        sb_adds(buf, ">");
    }
}

static void iopc_dump_field(sb_t *buf, const iopc_pkg_t *pkg,
                            const iopc_field_t *field,
                            unsigned flags)
{
    const char *prefix = "";

    if (is_name_reserved(field->name)) {
        prefix = "r#";
    }

    if (flags & ENUM_STYLE) {
        t_scope;

        sb_addf(buf, "%s%*pM(", prefix,
                LSTR_FMT_ARG(t_field_name_to_camelcase(field->name)));
        iopc_dump_field_type(buf, pkg, field, flags);
        sb_addc(buf, ')');
    } else {
        t_scope;

        sb_addf(buf, "%s%*pM: ", prefix,
                LSTR_FMT_ARG(t_camelcase_to_c(LSTR(field->name))));
        iopc_dump_field_type(buf, pkg, field, flags);
    }
}

static void iopc_dump_field_defval(sb_t *buf, const iopc_pkg_t *pkg,
                                   const iopc_field_t *field,
                                   unsigned flags)
{
    const char *prefix = "";

    if (is_name_reserved(field->name)) {
        prefix = "r#";
    }

    if (flags & ENUM_STYLE) {
        t_scope;

        sb_addf(buf, "%s%*pM(", prefix,
                LSTR_FMT_ARG(t_field_name_to_camelcase(field->name)));
    } else {
        t_scope;

        sb_addf(buf, "%s%*pM: ", prefix,
                LSTR_FMT_ARG(t_camelcase_to_c(LSTR(field->name))));
    }
    /* FIXME: this needs testing */
    if (field->repeat == IOP_R_DEFVAL) {
        switch (field->kind) {
          case IOP_T_I8: case IOP_T_I16: case IOP_T_I32: case IOP_T_I64:
            sb_addf(buf, "%jd", *(int64_t *)&field->defval.u64);
            break;
          case IOP_T_U8: case IOP_T_U16: case IOP_T_U32: case IOP_T_U64:
            sb_addf(buf, "%ju", field->defval.u64);
            break;
          case IOP_T_ENUM:
            /* TODO: find the right enum string from the value in
             * defval.u64 */
            sb_adds(buf, "Default::default()");
            break;
          case IOP_T_BOOL:
            sb_adds(buf, field->defval.u64 ? "true" : "false");
            break;
          case IOP_T_DOUBLE:
            sb_addf(buf, DOUBLE_FMT, field->defval.d);
            break;
          case IOP_T_STRING:
          case IOP_T_XML:
          case IOP_T_DATA:
            /* FIXME: this is buggy, the string must be escaped. */
            sb_addf(buf, "\"%s\"", (const char *)field->defval.ptr);
            break;
          default:
            assert (false);
            sb_adds(buf, "Default::default()");
            break;
        }
    } else {
        sb_adds(buf, "Default::default()");
    }

    if (flags & ENUM_STYLE) {
        sb_addc(buf, ')');
    }
}

static void iopc_dump_struct(sb_t *buf, const char *indent,
                             const iopc_pkg_t *pkg, iopc_struct_t *st,
                             const char *st_name)
{
    bool is_class = iopc_is_class(st->type);
    int next_tag = 1;

    if (!st_name) {
        st_name = st->name;
    }

    iopc_struct_sort_fields(st, BY_TAG);

    sb_addf(buf, "%s#[derive(Clone, Serialize, Deserialize)]\n", indent);
    sb_addf(buf, "%spub struct %s%s {", indent, st_name,
            is_class ? "Obj" : "");
    tab_for_each_entry(field, &st->fields) {
        while (field->tag > next_tag++) {
            sb_addf(buf, "\n%s    pub _dummy%d: (),", indent, next_tag - 1);
        }

        sb_addf(buf, "\n%s    pub ", indent);
        iopc_dump_field(buf, pkg, field, 0);
        sb_addc(buf, ',');
    }
    sb_addf(buf, "%s%s}\n", st->fields.len ? "\n" : "", indent);

    /* Impl default trait to set default values set in IOP, but also
     * initialize dummy fields */
    /* TODO: if there is no field with an IOP default value, use derive
     * default. */
    sb_addf(buf, "%simpl Default for %s {\n", indent, st_name);
    sb_addf(buf, "%s    fn default() -> Self {\n", indent);
    sb_addf(buf, "%s        Self {", indent);

    next_tag = 1;
    tab_for_each_entry(field, &st->fields) {
        while (field->tag > next_tag++) {
            sb_addf(buf, "\n%s            _dummy%d: (),", indent,
                    next_tag - 1);
        }

        sb_addf(buf, "\n%s            ", indent);
        iopc_dump_field_defval(buf, pkg, field, 0);
        sb_addc(buf, ',');
    }
    sb_addf(buf, "\n%s        }\n", indent);
    sb_addf(buf, "%s    }\n", indent);
    sb_addf(buf, "%s}\n", indent);

    if (iopc_is_class(st->type)) {
        sb_addf(buf, "%spub trait %s {}\n", indent, st_name);

        if (st->extends.len) {
            const iopc_struct_t *parent = st->extends.tab[0]->st;

            sb_addf(buf, "%simpl %s for %s {}\n", indent, parent->name,
                    st_name);
        }
    }
}

static void iopc_dump_union(sb_t *buf, const char *indent,
                            const iopc_pkg_t *pkg, iopc_struct_t *st,
                            const char *st_name)
{
    if (!st_name) {
        st_name = st->name;
    }

    iopc_struct_sort_fields(st, BY_TAG);

    sb_addf(buf, "%s#[derive(Clone, Serialize, Deserialize)]\n", indent);
    sb_addf(buf, "%spub enum %s {", indent, st_name);
    tab_for_each_entry(field, &st->fields) {
        sb_addf(buf, "\n%s    ", indent);
        iopc_dump_field(buf, pkg, field, ENUM_STYLE);
        sb_adds(buf, ",");
    }
    sb_addf(buf, "\n%s}\n", indent);

    /* FIXME: Can this test even be false? */
    if (st->fields.len > 0) {
        /* In order to have a default initializer for structs, we need one for
         * enums as well (as they can be used as struct fields). */
        sb_addf(buf, "%simpl Default for %s {\n", indent, st_name);
        sb_addf(buf, "%s    fn default() -> Self {\n", indent);
        sb_addf(buf, "%s        %s::", indent, st_name);
        iopc_dump_field_defval(buf, pkg, st->fields.tab[0], ENUM_STYLE);
        sb_addf(buf, "\n%s    }\n", indent);
        sb_addf(buf, "%s}\n", indent);
    }
}

static void iopc_dump_structs(sb_t *buf, iopc_pkg_t *pkg)
{
    tab_for_each_entry(st, &pkg->structs) {
        switch (st->type) {
          case STRUCT_TYPE_STRUCT:
          case STRUCT_TYPE_CLASS:
            iopc_dump_struct(buf, "", pkg, st, NULL);
            sb_addc(buf, '\n');
            break;

          case STRUCT_TYPE_UNION:
            iopc_dump_union(buf, "", pkg, st, NULL);
            sb_addc(buf, '\n');
            break;

          default:
            break;
        }
    }
}

/* }}} */
/* Iface {{{ */

static void iopc_dump_rpc(sb_t *buf, const iopc_pkg_t *pkg,
                          const iopc_fun_t *rpc)
{
    t_scope;
    lstr_t name = t_field_name_to_camelcase(rpc->name);

    if (rpc->arg) {
        if (rpc->arg_is_anonymous) {
            iopc_dump_struct(buf, "        ", pkg, rpc->arg,
                             t_fmt("%pLArgs", &name));
        } else {
            sb_adds(buf, "        #[derive(Serialize, Deserialize)]\n");
            sb_addf(buf, "        pub struct %pLArgs(pub ", &name);
            iopc_dump_field_basetype(buf, pkg, rpc->farg);
            sb_adds(buf, ");\n");
        }
    } else {
        sb_addf(buf, "        pub type %pLArgs = ();\n", &name);
    }

    if (rpc->res) {
        if (rpc->res_is_anonymous) {
            iopc_dump_struct(buf, "        ", pkg, rpc->res,
                             t_fmt("%pLRes", &name));
        } else {
            sb_adds(buf, "        #[derive(Serialize, Deserialize)]\n");
            sb_addf(buf, "        pub struct %pLRes(pub ", &name);
            iopc_dump_field_basetype(buf, pkg, rpc->fres);
            sb_adds(buf, ");\n");
        }
    } else {
        sb_addf(buf, "        pub type %pLRes = ();\n", &name);
    }

    if (rpc->exn) {
        if (rpc->exn_is_anonymous) {
            iopc_dump_struct(buf, "        ", pkg, rpc->exn,
                             t_fmt("%pLExn", &name));
        } else {
            sb_adds(buf, "        #[derive(Serialize, Deserialize)]\n");
            sb_addf(buf, "        pub struct %pLExn(pub ", &name);
            iopc_dump_field_basetype(buf, pkg, rpc->fexn);
            sb_adds(buf, ");\n");
        }
    } else {
        sb_addf(buf, "        pub type %pLExn = ();\n", &name);
    }

    /* Add dummy type as trait guard */
    sb_addf(buf, "        pub struct %pL {}\n", &name);

    /* Impl RPC trait for this type */
    sb_addf(buf, "        impl libcommon_ic::types::Rpc for %pL {\n", &name);
    sb_addf(buf, "            type Input = %pLArgs;\n", &name);
    sb_addf(buf, "            type Output = %pLRes;\n", &name);
    sb_addf(buf, "            type Exception = %pLExn;\n", &name);
    sb_addf(buf, "            const TAG: u16 = %d;\n", rpc->tag);
    sb_addf(buf, "            const ASYNC: bool = %s;\n",
            rpc->fun_is_async ? "true" : "false");
    sb_adds(buf, "        }\n");
}

static void iopc_dump_iface(sb_t *buf, const iopc_pkg_t *pkg,
                            iopc_iface_t *iface)
{
    t_scope;
    bool first = true;
    lstr_t name = t_camelcase_to_c(LSTR(iface->name));

    sb_addf(buf, "    pub mod %pL {\n", &name);
    sb_addf(buf, "        use super::super::*;\n");
    sb_addf(buf, "        use libcommon_ic;\n\n");

    tab_for_each_entry(rpc, &iface->funs) {
        if (first) {
            first = false;
        } else {
            sb_addc(buf, '\n');
        }
        iopc_dump_rpc(buf, pkg, rpc);
    }

    sb_addf(buf, "    }\n");
}

static void iopc_dump_ifaces(sb_t *buf, iopc_pkg_t *pkg)
{
    bool first = true;

    sb_adds(buf, "pub mod rpcs {\n");

    tab_for_each_entry(iface, &pkg->ifaces) {
        switch (iface->type) {
          case IFACE_TYPE_IFACE:
            if (first) {
                first = false;
            } else {
                sb_addc(buf, '\n');
            }
            iopc_dump_iface(buf, pkg, iface);
            break;

          default:
            break;
        }
    }

    sb_adds(buf, "}\n");
}

/* }}} */
/* Module {{{ */

static void iopc_dump_modules(sb_t *buf, iopc_pkg_t *pkg)
{
    if (pkg->modules.len == 0) {
        return;
    }

    sb_adds(buf, "\npub mod modules {\n");
    tab_for_each_entry(mod, &pkg->modules) {
        t_scope;

        sb_addf(buf, "    pub mod %*pM {\n",
                LSTR_FMT_ARG(t_camelcase_to_c(LSTR(mod->name))));

        for (int i = 0; i < mod->fields.len; i++) {
            const iopc_field_t *f = mod->fields.tab[i];
            lstr_t fname = t_camelcase_to_c(LSTR(f->name));

            lstr_ascii_toupper(&fname);
            sb_addf(buf, "        pub const %pL: u16 = %d;\n", &fname,
                    f->tag);
        }
        sb_adds(buf, "    }\n");
    }

    sb_adds(buf, "}\n");
}

/* }}} */

int iopc_do_rust(iopc_pkg_t *pkg, const char *outdir, sb_t *depbuf)
{
    SB_8k(buf);
    char path[PATH_MAX];

    iopc_set_path(outdir, pkg, ".rs", sizeof(path), path, true);

    sb_adds(&buf, RO_WARN);
    sb_adds(&buf, "use serde_iop::{Serialize, Deserialize};\n");
    if (pkg->enums.len > 0) {
        sb_adds(&buf,
                "use serde_repr::{Serialize_repr, Deserialize_repr};\n");
    }

    iopc_dump_imports(&buf, pkg);

    iopc_dump_enums(&buf, pkg);
    iopc_dump_structs(&buf, pkg);
    iopc_dump_ifaces(&buf, pkg);
    iopc_dump_modules(&buf, pkg);

    qv_deep_wipe(&pp_g, p_delete);
    return iopc_write_file(&buf, path);
}