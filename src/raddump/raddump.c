// Copyright (c) 2024 Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

internal int
rd_marker_is_before(void *a, void *b)
{
  return u64_is_before(&((RD_Marker*)a)->off, &((RD_Marker*)b)->off);
}

internal RD_MarkerArray *
rd_section_markers_from_coff_symbol_table(Arena *arena, String8 raw_data, U64 string_table_off, U64 section_count, COFF_Symbol32Array symbols)
{
  Temp scratch = scratch_begin(&arena, 1);

  // extract markers from symbol table
  RD_MarkerList *markers = push_array(scratch.arena, RD_MarkerList, section_count);
  for (U64 symbol_idx = 0; symbol_idx < symbols.count; ++symbol_idx) {
    COFF_Symbol32 *symbol = &symbols.v[symbol_idx];

    COFF_SymbolValueInterpType interp = coff_interp_symbol(symbol->section_number, symbol->value, symbol->storage_class);
    B32 is_marker = interp == COFF_SymbolValueInterp_REGULAR &&
                    symbol->aux_symbol_count == 0 &&
                    (symbol->storage_class == COFF_SymStorageClass_EXTERNAL || symbol->storage_class == COFF_SymStorageClass_STATIC);

    if (is_marker) {
      String8 name = coff_read_symbol_name(raw_data, string_table_off, &symbol->name);

      RD_MarkerNode *n = push_array(scratch.arena, RD_MarkerNode, 1);
      n->v.off         = symbol->value;
      n->v.string      = name;

      RD_MarkerList *list = &markers[symbol->section_number-1];
      SLLQueuePush(list->first, list->last, n);
      ++list->count;
    }

    symbol_idx += symbol->aux_symbol_count;
  }

  // lists -> arrays
  RD_MarkerArray *result = push_array(arena, RD_MarkerArray, section_count);
  for (U64 i = 0; i < section_count; ++i) {
    result[i].count = 0;
    result[i].v     = push_array(arena, RD_Marker, markers[i].count);
    for (RD_MarkerNode *n = markers[i].first; n != 0; n = n->next) {
      result[i].v[result[i].count++] = n->v;
    }
  }

  // sort arrays
  for (U64 i = 0; i < section_count; ++i) {
    radsort(result[i].v, result[i].count, rd_marker_is_before);
  }

  scratch_end(scratch);
  return result;
}

////////////////////////////////
//~ Disasm

internal RD_DisasmResult
rd_disasm_next_instruction(Arena *arena, Arch arch, U64 addr, String8 raw_code)
{
  RD_DisasmResult result = {0};

  switch (arch) {
    case Arch_Null: break;

    case Arch_x64:
    case Arch_x86: {
      ZydisMachineMode             machine_mode = bit_size_from_arch(arch) == 32 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LONG_64;
      ZydisDisassembledInstruction inst         = {0};
      ZyanStatus                   status       = ZydisDisassemble(machine_mode, addr, raw_code.str, raw_code.size, &inst, ZYDIS_FORMATTER_STYLE_INTEL);

      String8 text = str8_cstring_capped(inst.text, inst.text+sizeof(inst.text));
      result.text = push_str8_copy(arena, text);
      result.size = inst.info.length;
    } break;

    default: NotImplemented;
  }

  return result;
}

internal void
rd_format_disasm(Arena            *arena,
                 String8List      *out,
                 String8           indent,
                 Arch              arch,
                 U64               image_base,
                 U64               sect_off,
                 U64               marker_count,
                 RD_Marker        *markers,
                 String8           raw_code)
{
  Temp scratch = scratch_begin(&arena, 1);

  U64 bytes_buffer_max = 256;
  U8 *bytes_buffer     = push_array(scratch.arena, U8, bytes_buffer_max);

  U64     decode_off    = 0;
  U64     marker_cursor = 0;
  String8 to_decode     = raw_code;

  for (; to_decode.size > 0; ) {
    Temp temp = temp_begin(scratch.arena);

    // decode instruction
    U64             addr          = image_base + sect_off + decode_off;
    RD_DisasmResult disasm_result = rd_disasm_next_instruction(temp.arena, arch, addr, to_decode);

    // format instruction bytes
    String8 bytes;
    {
      U64 bytes_size = 0;
      for (U64 i = 0; i < disasm_result.size; ++i) {
        bytes_size += raddbg_snprintf(bytes_buffer + bytes_size, bytes_buffer_max-bytes_size, "%s%02x", i > 0 ? " " : "", to_decode.str[i]);
      }
      bytes = str8(bytes_buffer, bytes_size);
    }

    // print address marker
    if (marker_cursor < marker_count) {
      RD_Marker *m = &markers[marker_cursor];
      // NOTE: markers must be sorted on address
      if (decode_off <= m->off && m->off < decode_off + disasm_result.size) {
        if (m->off != decode_off) {
          U64 off = m->off - decode_off;
          rd_printf("; %S+%#llx", m->string, addr);
        } else {
          rd_printf("; %S", m->string);
        }
        marker_cursor += 1;
      }
    }

    // print final line
    rd_printf("%#08x: %-32S %S", addr, bytes, disasm_result.text);

    // advance
    to_decode = str8_skip(to_decode, disasm_result.size);
    decode_off += disasm_result.size;

    temp_end(temp);
  }

  scratch_end(scratch);
}

////////////////////////////////
//~ Raw Data

internal String8
rd_format_hex_array(Arena *arena, U8 *ptr, U64 size)
{
  U64  buf_max  = size * 3;
  U8  *buf      = push_array(arena, U8, buf_max);
  U64  buf_size = 0;

  for (U64 i = 0; i < size; ++i) {
    buf_size += raddbg_snprintf(buf+buf_size, buf_max-buf_size, "%s%02x", i>0 ? " " : "", ptr[i]);
  }

  buf[buf_size] = '\0';

  String8 result = str8(buf, buf_size);
  return result;
}

internal void
rd_format_raw_data(Arena       *arena,
                   String8List *out,
                   String8      indent,
                   U64          bytes_per_row,
                   U64          marker_count,
                   RD_Marker   *markers,
                   String8      raw_data)
{
  AssertAlways(bytes_per_row > 0);

  U8 temp_buffer[1024];

  String8 to_format = raw_data;
  for (; to_format.size > 0; ) {
    String8 raw_row = str8_prefix(to_format, bytes_per_row);

    U64 temp_cursor = 0;

    // offset
    U64 offset = (U64)(raw_row.str-raw_data.str);
    temp_cursor += raddbg_snprintf(temp_buffer+temp_cursor, sizeof(temp_buffer)-temp_cursor, "%#08x: ", offset);

    // hex
    for (U64 i = 0; i < raw_row.size; ++i) {
      U8 b = raw_row.str[i];
      temp_cursor += raddbg_snprintf(temp_buffer+temp_cursor, sizeof(temp_buffer)-temp_cursor, "%s%02x", i>0 ? " " : "", b);
    }
    U64 hex_indent_size = (bytes_per_row - raw_row.size) * 3;
    MemorySet(temp_buffer+temp_cursor, ' ', hex_indent_size);
    temp_cursor += hex_indent_size;

    temp_cursor += raddbg_snprintf(temp_buffer+temp_cursor, sizeof(temp_buffer) - temp_cursor, " ");

    // ascii
    for (U64 i = 0; i < raw_row.size; ++i) {
      U8 b = raw_row.str[i];
      U8 c = b;
      if (c < ' ' || c > '~') {
        c = '.';
      }
      temp_cursor += raddbg_snprintf(temp_buffer+temp_cursor, sizeof(temp_buffer)-temp_cursor, "%c", c);
    }

    rd_printf("%.*s", temp_cursor, temp_buffer);

    // advance
    to_format = str8_skip(to_format, bytes_per_row);
  }
}

//- CodeView

internal void
cv_format_binary_annots(Arena *arena, String8List *out, String8 indent, CV_Arch arch, String8 raw_data)
{
  if (raw_data.size) {
    Temp scratch = scratch_begin(&arena, 1);

    rd_printf("Binary Annotations:");
    rd_indent();

    U64 cursor = 0;
    for (; cursor < raw_data.size; ) {
      String8List op_list = {0};

      U8 op;
      cursor += str8_deserial_read_struct(raw_data, cursor, &op);
      if (op == CV_InlineBinaryAnnotation_Null) {
        break;
      }

      U8 params[2];
      U32 param_count = (op == CV_InlineBinaryAnnotation_ChangeCodeOffsetAndLineOffset) ? 2 : 1;
      cursor += str8_deserial_read_array(raw_data, cursor, &params[0], param_count);

      String8 opcode_str = cv_string_from_binary_opcode(op);
      str8_list_pushf(scratch.arena, &op_list, "%S", opcode_str);
      for (U32 i = 0; i < param_count; ++i) {
        str8_list_pushf(scratch.arena, &op_list, " %x", params[i]);
      }

      String8 op_str = str8_list_join(scratch.arena, &op_list, &(StringJoin){.sep=str8_lit(" ")});
      rd_printf("%S", op_str);
    }
    rd_unindent();

    rd_printf("Binary Annotations Length: %u bytes (%u bytes padding)", raw_data.size, raw_data.size - cursor);

    scratch_end(scratch);
  }
}

internal void
cv_format_lvar_addr_range(Arena *arena, String8List *out, String8 indent, CV_LvarAddrRange range)
{
  rd_printf("Address Range: %04x:%08x Size: %#x", range.sec, range.off, range.len);
}

internal void
cv_format_lvar_addr_gap(Arena *arena, String8List *out, String8 indent, String8 raw_data)
{
  U64 count = raw_data.size / sizeof(CV_LvarAddrGap);
  if (count > 0) {
    U64 cursor = 0;
    rd_printf("# Address Gaps");
    rd_indent();
    for (U64 i = 0; i < count; ++i) {
      CV_LvarAddrGap gap = {0};
      cursor += str8_deserial_read_struct(raw_data, cursor, &gap);
      rd_printf("Off: %#x, Len %#x", gap.off, gap.len);
    }
    rd_unindent();
  }
}

internal void
cv_format_lvar_attr(Arena *arena, String8List *out, String8 indent, CV_LocalVarAttr attr)
{
  Temp scratch = scratch_begin(&arena,1);
  rd_printf("Address: %S", cv_string_sec_off(scratch.arena, attr.seg, attr.off));
  rd_printf("Flags:   %S", cv_string_from_local_flags(scratch.arena, attr.flags));
  scratch_end(scratch);
}

internal void
cv_format_symbol(Arena *arena, String8List *out, String8 indent, CV_Arch arch, U32 type, String8 raw_symbol)
{
  Temp scratch = scratch_begin(&arena, 1);
  U64 cursor = 0;
  switch (type) {
    case CV_SymKind_THUNK32_ST:
    case CV_SymKind_THUNK32: {
      CV_SymThunk32 sym   = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:    %S",         name);
      rd_printf("Parent:  %x",         sym.parent);
      rd_printf("End:     %x",         sym.end);
      rd_printf("Next:    %x",         sym.next);
      rd_printf("Address: %S",         cv_string_sec_off(scratch.arena, sym.sec, sym.off));
      rd_printf("Length:  %u (bytes)", sym.len);
      rd_printf("Ordinal: %S",         cv_string_from_thunk_ordinal(sym.ord));
    } break;
    case CV_SymKind_FILESTATIC: {
      CV_SymFileStatic sym   = {0};
      String8          name = str8_zero();
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);
      rd_printf("Name:  %S", name);
      rd_printf("Type:  %S", cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("Flags: %S", cv_string_from_local_flags(scratch.arena, sym.flags));
    } break;
    case CV_SymKind_CALLERS:
    case CV_SymKind_CALLEES: {
      CV_SymFunctionList sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      CV_TypeIndex *funcs = push_array(scratch.arena, CV_TypeIndex, sym.count);
      cursor += str8_deserial_read_array(raw_symbol, cursor, &funcs[0], sym.count);
      U32  invocation_count = (raw_symbol.size - cursor) / sizeof(U32);
      U32 *invocations      = push_array(arena, U32, invocation_count);
      cursor += str8_deserial_read_array(raw_symbol, cursor, &invocations[0], invocation_count);

      rd_printf("Count: %u", sym.count);
      rd_indent();
      for (U32 i = 0; i < sym.count; ++i) {
        U32 invoks = i < invocation_count ? invocations[i] : 0;
        rd_printf("%08x (%u)", funcs[i], invoks);
      }
      rd_unindent();
    } break;
    case CV_SymKind_INLINEES: {
      CV_SymInlinees sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Count: %u", sym.count);
      rd_indent();
      for (U32 i = 0; i < sym.count; ++i) {
        U32 itype;
        cursor += str8_deserial_read_struct(raw_symbol, cursor, &itype);
        rd_printf("%S", cv_string_from_itype(arena, itype));
      }
      rd_unindent();
    } break;
    case CV_SymKind_INLINESITE: {
      CV_SymInlineSite sym         = {0};
      String8          raw_annots = str8_skip(raw_symbol, sizeof(CV_SymInlineSite));
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += raw_annots.size;

      rd_printf("Parent:  %#x", sym.parent);
      rd_printf("End:     %#x", sym.end);
      rd_printf("Inlinee: %S",  cv_string_from_itemid(arena, sym.inlinee));
      cv_format_binary_annots(arena, out, indent, arch, raw_annots);
    } break;
    case CV_SymKind_INLINESITE2: {
      CV_SymInlineSite2 sym         = {0};
      String8           raw_annots = str8_skip(raw_symbol, sizeof(CV_SymInlineSite2));
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += raw_annots.size;

      rd_printf("Parent:      %#x", sym.parent_off);
      rd_printf("End:         %#x", sym.end_off);
      rd_printf("Inlinee:     %S",  cv_string_from_itemid(arena, sym.inlinee));
      rd_printf("Invocations: %u",  sym.invocations);
      cv_format_binary_annots(arena, out, indent, arch, raw_annots);
    } break;
    case CV_SymKind_INLINESITE_END: {
      // nothing to report
    } break;
    case CV_SymKind_LTHREAD32_ST:
    case CV_SymKind_GTHREAD32_ST:
    case CV_SymKind_LTHREAD32:
    case CV_SymKind_GTHREAD32: {
      CV_SymThread32 sym   = {0};
      String8        name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:        %S", name);
      rd_printf("TSL Address: %S", cv_string_sec_off(scratch.arena, sym.tls_seg, sym.tls_off));
      rd_printf("Type:        %S", cv_string_from_itype(scratch.arena, sym.itype));
    } break;
    case CV_SymKind_OBJNAME: {
      CV_SymObjName sym   = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:      %S",  name);
      rd_printf("Signature: %#x", sym.sig);
    } break;
    case CV_SymKind_BLOCK32_ST:
    case CV_SymKind_BLOCK32: {
      CV_SymBlock32 sym   = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Parent:  %x", sym.parent);
      rd_printf("End:     %x", sym.end);
      rd_printf("Address: %S", cv_string_sec_off(scratch.arena, sym.sec, sym.off));
      rd_printf("Length:  %u (bytes)", sym.len);
    } break;
    case CV_SymKind_LABEL32_ST:
    case CV_SymKind_LABEL32: {
      CV_SymLabel32 sym = {0};
      String8 name = str8_zero();
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Address: %S", cv_string_sec_off(scratch.arena, sym.sec, sym.off));
      rd_printf("Flags:   %S", cv_string_from_proc_flags(scratch.arena, sym.flags));
    } break;
    case CV_SymKind_COMPILE: {
      Assert(!"TODO: test");
      CV_SymCompile sym            = {0};
      String8       version_string = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &version_string);

      U32     language     = CV_CompileFlags_Extract_Language(sym.flags);
      U32     float_prec   = CV_CompileFlags_ExtractFloatPrec(sym.flags);
      U32     float_pkg    = CV_CompileFlags_ExtractFloatPkg(sym.flags);
      U32     ambient_data = CV_CompileFlags_ExtractAmbientData(sym.flags);
      U32     mode         = CV_CompileFlags_ExtractMode(sym.flags);
      rd_printf("Arch:           %S", cv_string_from_arch(sym.machine));
      rd_printf("Language:       %S", cv_string_from_language(language));
      rd_printf("FloatPrec:      %x", float_prec);
      rd_printf("FloatPkg:       %x", float_pkg);
      rd_printf("Ambient Data:   %x", ambient_data);
      rd_printf("Mode:           %x", mode);
      rd_printf("Version String: %S", version_string);
    } break;
    case CV_SymKind_COMPILE2_ST:
    case CV_SymKind_COMPILE2: {
      Assert(!"TODO: test");
      CV_SymCompile2 sym            = {0};
      String8        version_string = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &version_string);

      U32 language = CV_Compile2Flags_Extract_Language(sym.flags);
      rd_printf("Machine:          %S",    cv_string_from_arch(sym.machine));
      rd_printf("Flags:            %x",    sym.flags);
      rd_printf("Language:         %S",    cv_string_from_language(language));
      rd_printf("Frontend Version: %u.%u", sym.ver_fe_major, sym.ver_fe_minor);
      rd_printf("Frontend Build:   %u",    sym.ver_fe_build);
      rd_printf("Backend Version:  %u.%u", sym.ver_major, sym.ver_minor);
      rd_printf("Backend Build:    %u",    sym.ver_build);
      rd_printf("Version String:   %S",    version_string);
    } break;
    case CV_SymKind_COMPILE3: {
      CV_SymCompile3 sym            = {0};
      String8        version_string = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &version_string);

      U32 language = CV_Compile3Flags_Extract_Language(sym.flags);
      rd_printf("Machine:          %S",    cv_string_from_arch(sym.machine));
      rd_printf("Flags:            %x",    sym.flags);
      rd_printf("Language:         %S",    cv_string_from_language(language));
      rd_printf("Frontend Version: %u.%u", sym.ver_fe_major, sym.ver_fe_minor);
      rd_printf("Frontend Build:   %u",    sym.ver_fe_build);
      rd_printf("Fontend QFE:      %u",    sym.ver_feqfe);
      rd_printf("Backend Version:  %u.%u", sym.ver_major, sym.ver_minor);
      rd_printf("Backend Build:    %u",    sym.ver_build);
      rd_printf("Backend QFE:      %u",    sym.ver_qfe);
      rd_printf("Version String:   %S",    version_string);
    } break;
    case CV_SymKind_GPROC32_ST:
    case CV_SymKind_LPROC32_ST:
    case CV_SymKind_GPROC32_ID:
    case CV_SymKind_LPROC32_ID:
    case CV_SymKind_LPROC32:
    case CV_SymKind_GPROC32: {
      CV_SymProc32 sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      String8 name = str8_zero();
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:        %S",         name);
      rd_printf("Parent:      %#x",        sym.parent);
      rd_printf("End:         %#x",        sym.end);
      rd_printf("Next:        %#x",        sym.next);
      rd_printf("Length:      %u (bytes)", sym.len);
      rd_printf("Debug Start: %#x",        sym.dbg_start);
      rd_printf("Debug End:   %#x",        sym.dbg_end);
      rd_printf("Type:        %S",         cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("Address:     %S",         cv_string_sec_off(scratch.arena, sym.sec, sym.off));
      rd_printf("Flags:       %S",         cv_string_from_proc_flags(scratch.arena, sym.flags));
    } break;
    case CV_SymKind_LDATA32_ST:
    case CV_SymKind_GDATA32_ST:
    case CV_SymKind_GDATA32:
    case CV_SymKind_LDATA32: {
      CV_SymData32 sym  = {0};
      String8      name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Type:    %S", cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("Address: %S", cv_string_sec_off(scratch.arena, sym.sec, sym.off));
    } break;
    case CV_SymKind_CONSTANT_ST:
    case CV_SymKind_CONSTANT: {
      CV_SymConstant   sym  = {0};
      CV_NumericParsed size = {0};
      String8          name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += cv_read_numeric(raw_symbol, cursor, &size);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name: %S", name);
      rd_printf("Type: %S", cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("Size: %S", cv_string_from_numeric(scratch.arena, size));
    } break;
    case CV_SymKind_FRAMEPROC: {
      CV_SymFrameproc sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      String8 flags     = cv_string_from_frame_proc_flags(scratch.arena, sym.flags);
      U32     local_ptr = CV_FrameprocFlags_Extract_LocalBasePointer(sym.flags);
      U32     param_ptr = CV_FrameprocFlags_Extract_ParamBasePointer(sym.flags);
      rd_printf("Frame Size:          %x",         sym.frame_size);
      rd_printf("Pad Size:            %x",         sym.pad_size);
      rd_printf("Pad Offset:          %x",         sym.pad_off);
      rd_printf("Save Registers Area: %u (bytes)", sym.save_reg_size);
      rd_printf("Exception Handler:   %S",         cv_string_sec_off(arena, sym.eh_sec, sym.eh_off));
      rd_printf("Flags:               %S",         flags);
      rd_printf("Local pointer:       %S",         cv_string_from_reg_id(arch, cv_map_encoded_base_pointer(arch, local_ptr)));
      rd_printf("Param pointer:       %S",         cv_string_from_reg_id(arch, cv_map_encoded_base_pointer(arch, param_ptr)));
    } break;
    case CV_SymKind_LOCAL: {
      CV_SymLocal sym  = {0};
      String8     name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:  %S", name);
      rd_printf("Type:  %S", cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("Flags: %S", cv_string_from_local_flags(scratch.arena, sym.flags));
    } break;
    case CV_SymKind_DEFRANGE: {
      CV_SymDefrange sym      = {0};
      String8        raw_gaps = str8_skip(raw_symbol, sizeof(CV_SymDefrange));
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += raw_gaps.size;

      rd_printf("Program: %#x", sym.program);
      cv_format_lvar_addr_range(arena, out, indent, sym.range);
      cv_format_lvar_addr_gap(arena, out, indent, raw_gaps);
    } break;
    case CV_SymKind_DEFRANGE_REGISTER: {
      CV_SymDefrangeRegister sym      = {0};
      String8                raw_gaps = str8_skip(raw_symbol, sizeof(CV_SymDefrangeRegisterRel));
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += raw_gaps.size;

      rd_printf("Register:   %S", cv_string_from_reg_id(arch, sym.reg));
      rd_printf("Attributes: %S", cv_string_from_range_attribs(scratch.arena, sym.attribs));
      cv_format_lvar_addr_range(arena, out, indent, sym.range);
      cv_format_lvar_addr_gap(arena, out, indent, raw_gaps);
    } break;
    case CV_SymKind_DEFRANGE_FRAMEPOINTER_REL: {
      CV_SymDefrangeFramepointerRel sym      = {0};
      String8                       raw_gaps = str8_skip(raw_symbol, sizeof(CV_SymDefrangeFramepointerRel));
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Offset: %#x", sym.off);
      cv_format_lvar_addr_range(arena, out, indent, sym.range);
      cv_format_lvar_addr_gap(arena, out, indent, raw_gaps);
    } break;
    case CV_SymKind_DEFRANGE_SUBFIELD_REGISTER: {
      CV_SymDefrangeSubfieldRegister sym      = {0};
      String8                        raw_gaps = str8_skip(raw_symbol, sizeof(CV_SymDefrangeSubfieldRegister));
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += raw_gaps.size;

      rd_printf("Register:      %S",  cv_string_from_reg_id(arch, sym.reg));
      rd_printf("Attributes:    %S",  cv_string_from_range_attribs(scratch.arena, sym.attribs));
      rd_printf("Parent Offset: %#x", sym.field_offset);
      cv_format_lvar_addr_range(arena, out, indent, sym.range);
      cv_format_lvar_addr_gap(arena, out, indent, raw_gaps);
    } break;
    case CV_SymKind_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE: {
      CV_SymDefrangeFramepointerRelFullScope sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      rd_printf("Offset: %#x", sym.off);
    } break;
    case CV_SymKind_DEFRANGE_REGISTER_REL: {
      CV_SymDefrangeRegisterRel sym      = {0};
      String8                   raw_gaps = str8_skip(raw_symbol, sizeof(CV_SymDefrangeRegisterRel));
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += raw_gaps.size;

      rd_printf("Flags:   %S", cv_string_from_defrange_register_rel_flags(scratch.arena, sym.flags));
      rd_printf("Address: %S", cv_string_from_reg_off(scratch.arena, arch, sym.reg, sym.reg_off));
      cv_format_lvar_addr_gap(arena, out, indent, raw_gaps);
    } break;
    case CV_SymKind_END:
    case CV_SymKind_PROC_ID_END: {
      // no data
    } break;
    case CV_SymKind_UDT_ST:
    case CV_SymKind_UDT: {
      CV_SymUDT sym  = {0};
      String8   name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name: %S", name);
      rd_printf("Type: %S", cv_string_from_itype(scratch.arena, sym.itype));
    } break;
    case CV_SymKind_BUILDINFO: {
      CV_SymBuildInfo sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      rd_printf("ID: %#x", sym.id);
    } break;
    case CV_SymKind_UNAMESPACE_ST:
    case CV_SymKind_UNAMESPACE: {
      String8 name = {0};
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name: %S", name);
    } break;
    case CV_SymKind_REGREL32_ST:
    case CV_SymKind_REGREL32: {
      CV_SymRegrel32 sym  = {0};
      String8        name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Address: %S", cv_string_from_reg_off(scratch.arena, arch, sym.reg, sym.reg_off));
      rd_printf("Type:    %S", cv_string_from_itype(scratch.arena, sym.itype));
    } break;
    case CV_SymKind_CALLSITEINFO: {
      CV_SymCallSiteInfo sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Address: %S",         cv_string_sec_off(scratch.arena, sym.sec, sym.off));
      rd_printf("Pad:     %u (bytes)", sym.pad);
      rd_printf("Type:    %S",         cv_string_from_itype(scratch.arena, sym.itype));
    } break;
    case CV_SymKind_FRAMECOOKIE: {
      CV_SymFrameCookie sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Address: %S",  cv_string_sec_off(arena, sym.reg, sym.off));
      rd_printf("Kind:    %S",  cv_string_from_frame_cookie_kind(sym.kind));
      rd_printf("Flags:   %#x", sym.flags); // TODO: llvm and cvinfo.h don't define these flags...
    } break;
    case CV_SymKind_HEAPALLOCSITE: {
      CV_SymHeapAllocSite sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      String8 addr  = cv_string_sec_off(arena, sym.sec, sym.off);
      String8 itype = cv_string_from_itype(arena, sym.itype);
      rd_printf("Address: %S", addr);
      rd_printf("Type:    %S", itype);
      rd_printf("Call instruction length: %x (bytes)", sym.call_inst_len);
    } break;
    case CV_SymKind_ALIGN: {
      // spec:
      // Unused data. Use the length field that precedes every symbol record
      // to skip this record. The pad bytes must be zero. For sstGlobalSym
      // and sstGlobalPub, the length of the pad field must be at least the
      // sizeof (long). There must be an S_Align symbol at the end of these
      // tables with a pad field containing 0xffffffff. The sstStaticSym table
      // does not have this requirement
    } break;
    case CV_SymKind_SKIP: {
      // Unused data, tools use this symbol to reserve space for future expansion
      // in incremental builds.
    } break;
    case CV_SymKind_ENDARG: {
      // spec:
      // This symbol specifies the end of symbol records used in formal arguments for a function. Use of
      // this symbol is optional for OMF and required for MIPS-compiled code. In OMF format, the end
      // of arguments can also be deduced from the fact that arguments for a function have a positive
      // offset from the frame pointer.
    } break;
    case CV_SymKind_CVRESERVE: {
      // Reserved for MS debugger
    } break;
    case CV_SymKind_SSEARCH: {
      Assert(!"TODO: test");
      
      CV_SymStartSearch sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Start Symbol: %#x", sym.start_symbol);
      rd_printf("Segment:      %#x", sym.segment);
    } break;
    case CV_SymKind_RETURN: {
      Assert(!"TODO: test");
      
      CV_SymReturn sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Flags: S", cv_string_from_generic_flags(scratch.arena, sym.flags));
      rd_printf("Style: S", cv_string_from_generic_style(sym.style));
      if (sym.style == CV_GenericStyle_REG) {
        U8 count = 0;
        cursor += str8_deserial_read_struct(raw_symbol, cursor, &count);

        rd_printf("Byte Count: %u", count);
        rd_printf("Data:");
        rd_indent();
        for (U8 i = 0; i < count; ++i) {
          U8 v;
          cursor += str8_deserial_read_struct(raw_symbol, cursor, &v);
          rd_printf("  %02x", v);
        }
        rd_unindent();
      }
    } break;
    case CV_SymKind_ENTRYTHIS: {
      Assert(!"TODO: test");
      
      U16 symbol_size = 0, symbol_type = 0;
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &symbol_size);
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &symbol_type);
      String8 raw_subsym = str8_skip(raw_symbol, cursor);

      cv_format_symbol(arena, out, indent, arch, type, raw_subsym);
    } break;
    case CV_SymKind_SLINK32: {
      Assert(!"ret");
      
      CV_SymSLink32 sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Frame Size: %x", sym.frame_size);
      rd_printf("Address:    %S", cv_string_from_reg_off(scratch.arena, arch, sym.reg, sym.offset));
    } break;
    case CV_SymKind_OEM: {
      Assert(!"TODO: test");
      
      CV_SymOEM sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      // TODO: Not clear what to do about user data that follows, are we supposed to assume that
      // rest of the range is it? 
      //
      // CV-spec doesn't even mention S_OEM just LF_OEM and cvdump.exe prints out type with guid...
      rd_printf("Type: %S", cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("ID:   %S", string_from_guid(scratch.arena, sym.id));
    } break;
    case CV_SymKind_VFTABLE32:{
      Assert(!"TODO: test");
      
      CV_SymVPath32 sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Root:    %S", cv_string_from_itype(scratch.arena, sym.root));
      rd_printf("Path:    %S", cv_string_from_itype(scratch.arena, sym.path));
      rd_printf("Address: %S", cv_string_sec_off(scratch.arena, sym.seg, sym.off));
    } break;
    case CV_SymKind_PUB32_ST:
    case CV_SymKind_PUB32: {
      CV_SymPub32 sym  = {0};
      String8     name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Flags:   %S", cv_string_from_pub32_flags(scratch.arena, sym.flags));
      rd_printf("Address: %S", cv_string_sec_off(scratch.arena, sym.sec, sym.off));
    } break;
    case CV_SymKind_BPREL32_ST:
    case CV_SymKind_BPREL32: {
      Assert(!"TODO: test");
      
      CV_SymBPRel32 sym  = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:   %S",  name);
      rd_printf("Offset: %#x", sym.off);
      rd_printf("Type:   %S",  cv_string_from_itype(scratch.arena, sym.itype));
    } break;
    case CV_SymKind_REGISTER: {
      Assert(!"TODO: test");
      
      CV_SymRegister sym  = {0};
      String8        name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:     %S", name);
      rd_printf("Register: %S", cv_string_from_reg_id(arch, sym.reg));
      rd_printf("Type:     %S", cv_string_from_itype(scratch.arena, sym.itype));
    } break;
    case CV_SymKind_PROCREF_ST:
    case CV_SymKind_DATAREF_ST:
    case CV_SymKind_LPROCREF_ST:
    case CV_SymKind_ANNOTATIONREF:
    case CV_SymKind_LPROCREF:
    case CV_SymKind_PROCREF:
    case CV_SymKind_DATAREF: {
      Assert(!"TODO: test");
      
      CV_SymRef2 sym  = {0};
      String8    name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name: %S",  name);
      rd_printf("SUC:  %#x", sym.suc_name);
      rd_printf("IMod: %#x", sym.imod);
      rd_printf("Symbol Stream Offset: %#x", sym.sym_off);
    } break;
    case CV_SymKind_SEPCODE: {
      Assert(!"TODO: test");
      
      CV_SymSepcode sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Parent:         %#x",        sym.parent);
      rd_printf("End:            %#x",        sym.end);
      rd_printf("Length:         %u (bytes)", sym.len);
      rd_printf("Flags:          %S",         cv_string_from_sepcode(scratch.arena, sym.flags));
      rd_printf("Address:        %S",         cv_string_sec_off(scratch.arena, sym.sec, sym.sec_off));
      rd_printf("Parent Address: %S",         cv_string_sec_off(scratch.arena, sym.sec_parent, sym.sec_parent_off));
    } break;
    case CV_SymKind_PARAMSLOT_ST:
    case CV_SymKind_LOCALSLOT_ST:
    case CV_SymKind_LOCALSLOT: {
      Assert(!"TODO: test");
      
      CV_SymSlot sym  = {0};
      String8    name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name: %S", name);
      rd_printf("Slot: %u", sym.slot_index);
      rd_printf("Type: %S", cv_string_from_itype(scratch.arena, sym.itype));
    } break;
    case CV_SymKind_TRAMPOLINE: {
      Assert(!"TODO: test");
      
      CV_SymTrampoline sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Type:       %S",         cv_string_from_trampoline_kind(sym.kind));
      rd_printf("Thunk Size: %u (bytes)", sym.thunk_size);
      rd_printf("Thunk:      %.*s",       cv_string_sec_off(scratch.arena, sym.thunk_sec, sym.thunk_sec_off));
      rd_printf("Target:     %.*s",       cv_string_sec_off(scratch.arena, sym.target_sec, sym.target_sec_off));
    } break;
    case CV_SymKind_POGODATA: {
      Assert(!"TODO: test");
      
      CV_SymPogoInfo sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Invocations:                          %u", sym.invocations);
      rd_printf("Dynamic instruction count:            %u", sym.dynamic_inst_count);
      rd_printf("Static instruction count:             %u", sym.static_inst_count);
      rd_printf("Post inline static instruction count: %u", sym.post_inline_static_inst_count);
    } break;
    case CV_SymKind_MANYREG: {
      Assert(!"TODO: test");
      
      CV_SymManyreg sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Type:      %S", cv_string_from_itype(arena, sym.itype));
      rd_printf("Reg Count: %u", sym.count);
      rd_printf("Regs:");
      rd_indent();
      for (U8 i = 0; i < sym.count; ++i) {
        U8 v = 0;
        cursor += str8_deserial_read_struct(raw_symbol, cursor, &v);
        rd_printf("%S", cv_string_from_reg_id(arch, v));
      }
      rd_unindent();
    } break;
    case CV_SymKind_MANYREG2_ST:
    case CV_SymKind_MANYREG2: {
      Assert(!"TODO: test");
      
      CV_SymManyreg sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Type:      %S", cv_string_from_itype(arena, sym.itype));
      rd_printf("Reg Count: %u", sym.count);
      rd_printf("Regs:");
      rd_indent();
      for (U16 i = 0; i < sym.count; ++i) {
        U16 v = 0;
        cursor += str8_deserial_read_struct(raw_symbol, cursor, &v);
        rd_printf("%S", cv_string_from_reg_id(arch, v));
      }
      rd_unindent();
    } break;
    case CV_SymKind_SECTION: {
      Assert(!"TODO: test");
      
      CV_SymSection sym  = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:            %S",         name);
      rd_printf("Index:           %u",         sym.sec_index);
      rd_printf("Align:           %u",         sym.align);
      rd_printf("Virtual Offset:  %#x",        sym.rva);
      rd_printf("Size:            %u (bytes)", sym.size);
      rd_printf("Characteristics: %S",         coff_string_from_section_flags(scratch.arena, sym.characteristics));
    } break;
    case CV_SymKind_ENVBLOCK: {
      CV_SymEnvBlock sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      for (; cursor < raw_symbol.size; ) {
        String8 id = str8_zero();
        cursor += str8_deserial_read_cstr(raw_symbol, cursor, &id);
        String8 path = str8_zero();
        cursor += str8_deserial_read_cstr(raw_symbol, cursor, &path);
        if (id.size == 0 && path.size == 0) {
          break;
        }
        rd_printf("%S = %S", id, path);
      }
    } break;
    case CV_SymKind_COFFGROUP: {
      Assert(!"TODO: test");
      
      CV_SymCoffGroup sym  = {0};
      String8         name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:            %S",         name);
      rd_printf("Size:            %u (bytes)", sym.size);
      rd_printf("Characteristics: %S",         coff_string_from_section_flags(scratch.arena, sym.characteristics));
      rd_printf("Address:         %S",         cv_string_sec_off(scratch.arena, sym.sec, sym.off));
    } break;
    case CV_SymKind_EXPORT: {
      Assert(!"TODO: test");
      
      CV_SymExport sym  = {0};
      String8      name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:    %S",  name);
      rd_printf("Ordinal: %#x", sym.ordinal);
      rd_printf("Flags:   %S",  cv_string_from_export_flags(scratch.arena, sym.flags));
    } break;
    case CV_SymKind_ANNOTATION: {
      Assert(!"TODO: test");
      
      CV_SymAnnotation sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Address: %S", cv_string_sec_off(scratch.arena, sym.seg, sym.off));
      rd_printf("Count:   %u", sym.count);
      rd_printf("Annotations:");
      rd_indent();
      for (U16 i = 0; i < sym.count; ++i) {
        String8 str = str8_zero();
        cursor += str8_deserial_read_cstr(raw_symbol, cursor, &str);
        rd_printf("%S", str);
      }
      rd_unindent();
    } break;
    case CV_SymKind_MANFRAMEREL:
    case CV_SymKind_ATTR_FRAMEREL: {
      Assert(!"TODO: test");
      
      CV_SymAttrFrameRel sym  = {0};
      String8            name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:   %S",  name);
      rd_printf("Offset: %#x", sym.off);
      rd_printf("Type:   %S",  cv_string_from_itype(scratch.arena, sym.itype));
      cv_format_lvar_attr(arena, out, indent, sym.attr);
    } break;
    case CV_SymKind_MANREGISTER:
    case CV_SymKind_ATTR_REGISTER: {
      Assert(!"TODO: test");
      
      CV_SymAttrReg sym  = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:     %S", name);
      rd_printf("Type:     %S", cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("Register: %S", cv_string_from_reg_id(arch, sym.reg));
      cv_format_lvar_attr(arena, out, indent, sym.attr);
    } break;
    case CV_SymKind_ATTR_REGREL: {
      Assert(!"TODO: test");
      
      CV_SymAttrRegRel sym  = {0};
      String8          name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:     %S", name);
      rd_printf("Type:     %S", cv_string_from_itype(scratch.arena, sym.itype));
      rd_printf("Address:  %S", cv_string_from_reg_off(scratch.arena, arch, sym.reg, sym.off));
      cv_format_lvar_attr(arena, out, indent, sym.attr);
    } break;
    case CV_SymKind_MANYREG_ST:
    case CV_SymKind_MANMANYREG:
    case CV_SymKind_ATTR_MANYREG: {
      Assert(!"TODO: test");
      
      CV_SymAttrManyReg sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      U8 *regs = push_array(scratch.arena, U8, sym.count);
      cursor += str8_deserial_read_array(raw_symbol, cursor, &regs[0], sym.count);
      String8 name = str8_zero();
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:      %S", name);
      rd_printf("Type:      %S", cv_string_from_itype(scratch.arena, sym.itype));
      cv_format_lvar_attr(arena, out, indent, sym.attr);
      rd_printf("Reg Count: %u", sym.count);
      rd_printf("Regs:");
      rd_indent();
      for (U8 i = 0; i < sym.count; ++i) {
        rd_printf("%S", cv_string_from_reg_id(arch, regs[i]));
      }
      rd_unindent();
    } break;
    case CV_SymKind_MOD_TYPEREF: {

      Assert(!"TODO: test");
      
      CV_SymModTypeRef sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      String8List flags_list = {0};
      if (sym.flags & CV_ModTypeRefFlag_None) {
        str8_list_pushf(scratch.arena, &flags_list, "No TypeRef");
      } else if (sym.flags & CV_ModTypeRefFlag_OwnTMR) {
        str8_list_pushf(scratch.arena, &flags_list, "/Z7 TypeRef, SN=%04X", sym.word0);
        if (sym.flags & CV_ModTypeRefFlag_OwnTMPCT) {
          str8_list_pushf(scratch.arena, &flags_list, "own PCH types");
        }
        if (sym.flags & CV_ModTypeRefFlag_RefTMPCT) {
          str8_list_pushf(scratch.arena, &flags_list, "reference PCH types in module %04X", (sym.word1+1));
        }
      } else {
        str8_list_pushf(scratch.arena, &flags_list, "/Zi TypeRef");
        if (sym.flags & CV_ModTypeRefFlag_OwnTM) {
          str8_list_pushf(scratch.arena, &flags_list, "SN=%04X (type), SN=%04X (ID)", sym.word0, sym.word1);
        }
        if (sym.flags & CV_ModTypeRefFlag_RefTM) {
          str8_list_pushf(scratch.arena, &flags_list, "shared with Module %04X", sym.word0+1);
        }
      }
      String8 flags_str = str8_list_join(scratch.arena, &flags_list, &(StringJoin){.sep=str8_lit(", ")});

      rd_printf("%S", flags_str);
    } break;
    case CV_SymKind_DISCARDED: {
      Assert(!"TODO: test");
      
      CV_SymDiscarded sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      U32 symbol_type = 0;
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &symbol_type);
      String8 raw_subsym = str8_skip(raw_symbol, cursor);

      rd_printf("Kind:             %x", sym.kind);
      rd_printf("File ID:          %x", sym.file_id);
      rd_printf("File Line Number: %u", sym.file_ln);
      rd_printf("# Discarded Symbol");
      cv_format_symbol(arena, out, indent, arch, symbol_type, raw_subsym);
    } break;
    case CV_SymKind_PDBMAP: {
      Assert(!"TODO: test");
      
      String8 from = {0};
      String8 to   = {0};
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &from);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &to);

      rd_printf("From: %S", from);
      rd_printf("To:   %S", to);
    } break;
    case CV_SymKind_FASTLINK: {
      Assert(!"TODO: test");
      
      CV_SymFastLink sym  = {0};
      String8        name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:  %S", name);
      rd_printf("Flags: %x", sym.flags);
      rd_printf("Type:  %S", cv_string_from_itype(arena, sym.itype));
    } break;
    case CV_SymKind_ARMSWITCHTABLE: {
      Assert(!"TODO: test");
      
      CV_SymArmSwitchTable sym = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);

      rd_printf("Base Address:   %S", cv_string_sec_off(scratch.arena, sym.sec_base,   sym.off_base));
      rd_printf("Branch Address: %S", cv_string_sec_off(scratch.arena, sym.sec_branch, sym.off_branch));
      rd_printf("Table Address:  %S", cv_string_sec_off(scratch.arena, sym.sec_table,  sym.off_table));
      rd_printf("Entry count:    %u", sym.entry_count);
      rd_printf("Switch Type:    %x", sym.kind);
    } break;
    case CV_SymKind_REF_MINIPDB: {
      Assert(!"TODO: test");
      
      CV_SymRefMiniPdb sym  = {0};
      String8          name = {0};
      cursor += str8_deserial_read_struct(raw_symbol, cursor, &sym);
      cursor += str8_deserial_read_cstr(raw_symbol, cursor, &name);

      rd_printf("Name:  %S", name);
      rd_printf("Flags: %x", sym.flags);
      rd_printf("IMod:  %04x", sym.imod);
      if (sym.flags & CV_RefMiniPdbFlag_UDT) {
        rd_printf("Type: %S", cv_string_from_itype(scratch.arena, (CV_TypeIndex)sym.data));
      } else {
        rd_printf("Coff ISect: %#x", sym.data);
      }
    } break;
    // COBOL
    case CV_SymKind_CEXMODEL32:
    case CV_SymKind_COBOLUDT_ST:
    case CV_SymKind_COBOLUDT:
    // Pascal
    case CV_SymKind_WITH32_ST:
    case CV_SymKind_WITH32:
    //~ 16bit
    case CV_SymKind_REGISTER_16t:
    case CV_SymKind_CONSTANT_16t:
    case CV_SymKind_UDT_16t:
    case CV_SymKind_OBJNAME_ST:
    case CV_SymKind_COBOLUDT_16t:
    case CV_SymKind_MANYREG_16t:
    case CV_SymKind_BPREL16:
    case CV_SymKind_LDATA16:
    case CV_SymKind_GDATA16:
    case CV_SymKind_PUB16:
    case CV_SymKind_LPROC16:
    case CV_SymKind_GPROC16:
    case CV_SymKind_THUNK16:
    case CV_SymKind_BLOCK16:
    case CV_SymKind_WITH16:
    case CV_SymKind_LABEL16:
    case CV_SymKind_CEXMODEL16:
    case CV_SymKind_VFTABLE16:
    case CV_SymKind_REGREL16:
    case CV_SymKind_TI16_MAX:
    //~ 16:32 memory model
    case CV_SymKind_BPREL32_16t:
    case CV_SymKind_LDATA32_16t:
    case CV_SymKind_GDATA32_16t:
    case CV_SymKind_PUB32_16t:
    case CV_SymKind_LPROC32_16t:
    case CV_SymKind_GPROC32_16t:
    case CV_SymKind_VFTABLE32_16t:
    case CV_SymKind_REGREL32_16t:
    case CV_SymKind_LTHREAD32_16t:
    case CV_SymKind_GTHREAD32_16t:
    case CV_SymKind_LPROCMIPS_16t:
    case CV_SymKind_GPROCMIPS_16t:
    // MIPS
    case CV_SymKind_LPROCMIPS_ST:
    case CV_SymKind_GPROCMIPS_ST:
    case CV_SymKind_LPROCMIPS:
    case CV_SymKind_GPROCMIPS:
    case CV_SymKind_LPROCIA64:
    case CV_SymKind_GPROCIA64:
    case CV_SymKind_LPROCMIPS_ID:
    case CV_SymKind_GPROCMIPS_ID:
    // Managed
    case CV_SymKind_TOKENREF:
    case CV_SymKind_GMANPROC_ST:
    case CV_SymKind_LMANPROC_ST:
    case CV_SymKind_LMANDATA_ST:
    case CV_SymKind_GMANDATA_ST:
    case CV_SymKind_MANFRAMEREL_ST:
    case CV_SymKind_MANREGISTER_ST:
    case CV_SymKind_MANSLOT_ST:
    case CV_SymKind_MANTYPREF:
    case CV_SymKind_MANMANYREG_ST:
    case CV_SymKind_MANREGREL_ST:
    case CV_SymKind_MANMANYREG2_ST:
    case CV_SymKind_MANMANYREG2:
    case CV_SymKind_MANREGREL:
    case CV_SymKind_MANSLOT:
    case CV_SymKind_MANCONSTANT:
    case CV_SymKind_LMANDATA:
    case CV_SymKind_GMANDATA:
    case CV_SymKind_GMANPROC:
    case CV_SymKind_LMANPROC:
    // HLSL
    case CV_SymKind_DEFRANGE_DPC_PTR_TAG:
    case CV_SymKind_DPC_SYM_TAG_MAP:
    case CV_SymKind_DEFRANGE_HLSL:
    case CV_SymKind_GDATA_HLSL:
    case CV_SymKind_LDATA_HLSL:
    case CV_SymKind_LPROC32_DPC:
    case CV_SymKind_LPROC32_DPC_ID:
    case CV_SymKind_GDATA_HLSL32:
    case CV_SymKind_LDATA_HLSL32:
    case CV_SymKind_GDATA_HLSL32_EX:
    case CV_SymKind_LDATA_HLSL32_EX: 
    // IA64
    case CV_SymKind_LPROCIA64_ID:
    case CV_SymKind_GPROCIA64_ID:
    // VS2005
    case CV_SymKind_DEFRANGE_2005:
    case CV_SymKind_DEFRANGE2_2005:
    case CV_SymKind_ST_MAX: 
    case CV_SymKind_RESERVED1:
    case CV_SymKind_RESERVED2:
    case CV_SymKind_RESERVED3:
    case CV_SymKind_RESERVED4: {
    } break;
  }
  scratch_end(scratch);
}

internal U64
cv_format_leaf(Arena *arena, String8List *out, String8 indent, CV_LeafKind kind, String8 raw_leaf)
{
  Temp scratch = scratch_begin(&arena, 1);
  U64 cursor = 0;
  switch (kind) {
    case CV_LeafKind_NOTYPE: {
      // empty
    } break;
    case CV_LeafKind_BITFIELD: {
      CV_LeafBitField lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Type:     %S", cv_string_from_itype(scratch.arena, lf.itype));
      rd_printf("Length:   %u", lf.len);
      rd_printf("Position: %u", lf.pos);
    } break;
    case CV_LeafKind_CLASS2:
    case CV_LeafKind_STRUCT2: {
      CV_LeafStruct2   lf  = {0};
      CV_NumericParsed size = {0};
      String8          name = str8_zero();
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &size);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:          %S", name);
      rd_printf("Fields:        %S", cv_string_from_itype(scratch.arena, lf.field_itype));
      rd_printf("Properties:    %S", cv_string_from_type_props(scratch.arena, lf.props));
      rd_printf("Derived:       %S", cv_string_from_itype(scratch.arena, lf.derived_itype));
      rd_printf("VShape:        %S", cv_string_from_itype(scratch.arena, lf.vshape_itype));
      rd_printf("Unknown1:      %x", lf.unknown1);
      rd_printf("Unknown2:      %x", lf.unknown2);
      if (lf.props & CV_TypeProp_HasUniqueName) {
        String8 unique_name = str8_zero();
        cursor += str8_deserial_read_cstr(raw_leaf, cursor, &unique_name);
        rd_printf("Unique Name:  %S", unique_name);
      }
    } break;
    case CV_LeafKind_PRECOMP_ST: 
    case CV_LeafKind_PRECOMP: { 
      CV_LeafPreComp lf  = {0};
      String8        name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:        %S", name);
      rd_printf("Start Index: %x", lf.start_index);
      rd_printf("Count:       %u", lf.count);
      rd_printf("Signature:   %x", lf.sig);
    } break;
    case CV_LeafKind_TYPESERVER2: {
      CV_LeafTypeServer2 lf  = {0};
      String8            name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:  %S", name);
      rd_printf("Sig70: %S", string_from_guid(arena, lf.sig70));
      rd_printf("Age:   %u", lf.age);
    } break;
    case CV_LeafKind_BUILDINFO: {
      CV_LeafBuildInfo lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Entry Count: %u", lf.count);
      rd_indent();
      for (U16 i = 0; i < lf.count; ++i) {
        CV_ItemId id = 0;
        cursor += str8_deserial_read_struct(raw_leaf, cursor, &id);
        rd_printf("%S", cv_string_from_itemid(scratch.arena, id));
      }
      rd_unindent();
    } break;
    case CV_LeafKind_MFUNC_ID: {
      CV_LeafMFuncId lf  = {0};
      String8        name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);
      rd_printf("Name:       %S", name);
      rd_printf("Owner Type: %S", cv_string_from_itype(scratch.arena, lf.owner_itype));
      rd_printf("Type:       %S", cv_string_from_itype(scratch.arena, lf.itype));
    } break;
    case CV_LeafKind_VFUNCTAB: {
      CV_LeafVFuncTab lf = {0};
      cursor += syms_based_range_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Type: %S", cv_string_from_itype(scratch.arena, lf.itype));
    } break;
    case CV_LeafKind_METHODLIST: {
      for (; cursor < raw_leaf.size; ) {
        CV_LeafMethodListMember ml = {0};
        cursor += str8_deserial_read_struct(raw_leaf, cursor, &ml);
        U32 mprop     = CV_FieldAttribs_Extract_MPROP(ml.attribs);
        B32 has_vbase = (mprop == CV_MethodProp_PureIntro) || (mprop == CV_MethodProp_Intro);
        U32 vbase     = 0;
        if (has_vbase) {
          cursor += str8_deserial_read_struct(raw_leaf, cursor, &vbase);
        }
        rd_printf("Attribs:      %S", cv_string_from_field_attribs(scratch.arena, ml.attribs));
        rd_printf("Type:         %S", cv_string_from_itype(scratch.arena, ml.itype));
        if (has_vbase) {
          rd_printf("Virtual Base: %x", vbase);
        }
      }
    } break;
    case CV_LeafKind_ONEMETHOD_ST: 
    case CV_LeafKind_ONEMETHOD: {
      CV_LeafOneMethod lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      U32 mprop     = CV_FieldAttribs_Extract_MPROP(lf.attribs);
      B32 has_vbase = (mprop == CV_MethodProp_PureIntro) || (mprop == CV_MethodProp_Intro);
      U32 vbase     = 0;
      if (has_vbase) {
        cursor += str8_deserial_read_struct(raw_leaf, cursor, &vbase);
      }
      String8 name = {0};
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);
      rd_printf("Name:          %S", name);
      rd_printf("Field Attribs: %S", cv_string_from_field_attribs(scratch.arena, lf.attribs));
      rd_printf("Type:          %S", cv_string_from_itype(scratch.arena, lf.itype));
      if (has_vbase) {
        rd_printf("Virtual Base:  %#x", vbase);
      }
    } break;
    case CV_LeafKind_METHOD_ST: 
    case CV_LeafKind_METHOD: {
      CV_LeafMethod lf  = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:      %S", name);
      rd_printf("Count:     %u", lf.count);
      rd_printf("Type List: %S", cv_string_from_itype(scratch.arena, lf.list_itype));
    } break;
    case CV_LeafKind_VBCLASS:
    case CV_LeafKind_IVBCLASS: {
      CV_LeafVBClass   lf         = {0};
      CV_NumericParsed vbptr_off   = {0};
      CV_NumericParsed vbtable_off = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &vbptr_off);
      cursor += cv_read_numeric(raw_leaf, cursor, &vbtable_off);

      rd_printf("Attribs:          %S", cv_string_from_field_attribs(scratch.arena, lf.attribs));
      rd_printf("Direct Base Type: %S", cv_string_from_itype(scratch.arena, lf.itype));
      rd_printf("Virtual Base Ptr: %S", cv_string_from_itype(scratch.arena, lf.vbptr_itype));
      rd_printf("vbpoff:           %S", cv_string_from_numeric(scratch.arena, vbptr_off));
      rd_printf("vbind:            %S", cv_string_from_numeric(scratch.arena, vbtable_off));
    } break;
    case CV_LeafKind_BCLASS: {
      CV_LeafBClass    lf    = {0};
      CV_NumericParsed offset = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &offset);

      rd_printf("Attribs: %S", cv_string_from_field_attribs(scratch.arena, lf.attribs));
      rd_printf("Type:    %S", cv_string_from_itype(scratch.arena, lf.itype));
      rd_printf("Offset:  %S", cv_string_from_numeric(scratch.arena, offset));
    } break;
    case CV_LeafKind_VTSHAPE: {
      CV_LeafVTShape lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Entry Count: %u", lf.count);
      rd_indent();
      for (U16 i = 0; i < lf.count; ++i) {
        U8 packed_kind = 0;
        str8_deserial_read_struct(raw_leaf, cursor + (i / 2), &packed_kind);
        U8 kind = (packed_kind >> ((i % 2)*4)) & 0xF;
        rd_printf("%S", cv_string_from_virtual_table_shape_kind(kind));
      }
      rd_unindent();
      cursor += (lf.count * sizeof(U8) + 1) / 2;
    } break;
    case CV_LeafKind_STMEMBER_ST: 
    case CV_LeafKind_STMEMBER: {
      CV_LeafStMember lf  = {0};
      String8         name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Attribs: %S", cv_string_from_field_attribs(scratch.arena, lf.attribs));
      rd_printf("Type:    %S", cv_string_from_itype(scratch.arena, lf.itype));
    } break;
    case CV_LeafKind_MFUNCTION: {
      CV_LeafMFunction lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Return Type:      %S", cv_string_from_itype(scratch.arena, lf.ret_itype));
      rd_printf("Class Type:       %S", cv_string_from_itype(scratch.arena, lf.class_itype));
      rd_printf("This Type:        %S", cv_string_from_itype(scratch.arena, lf.this_itype));
      rd_printf("Call Kind:        %S", cv_string_from_call_kind(lf.call_kind));
      rd_printf("Function Attribs: %S", cv_string_from_function_attribs(scratch.arena, lf.attribs));
      rd_printf("Argument Count:   %u", lf.arg_count);
      rd_printf("Argument Type:    %S", cv_string_from_itype(scratch.arena, lf.arg_itype));
    } break;
    #if 0
    case CV_LeafKind_SKIP_16t: {
      CV_LeafSkip_16t lf = {0};
      cursor += str8_deserial_read_struct(base, range, cursor, &lf);

      rd_printf("Type: %S", cv_string_from_itype(scratch.arena, lf.type));
    } break;
    #endif
    case CV_LeafKind_SKIP: {
      // ms-symbol-pdf:
      // This is used by incremental compilers to reserve space for indices.
      CV_LeafSkip lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      rd_printf("Type: %S", cv_string_from_itype(scratch.arena, lf.itype));
    } break;
    case CV_LeafKind_ENUM_ST: 
    case CV_LeafKind_ENUM: {
      CV_LeafEnum lf  = {0};
      String8     name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:        %S", name);
      rd_printf("Field Count: %u", lf.count);
      rd_printf("Properties:  %S", cv_string_from_type_props(scratch.arena, lf.props));
      rd_printf("Type:        %S", cv_string_from_itype(scratch.arena, lf.base_itype));
      rd_printf("Field:       %S", cv_string_from_itype(scratch.arena, lf.field_itype));
      if (lf.props & CV_TypeProp_HasUniqueName) {
        String8 unique_name = {0};
        cursor += str8_deserial_read_cstr(raw_leaf, cursor, &unique_name);
        rd_printf("Unique Name: %S", unique_name);
      }
    } break;
    case CV_LeafKind_ENUMERATE: {
      CV_LeafEnumerate lf   = {0};
      CV_NumericParsed value = {0};
      String8          name  = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &value);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Attribs: %S", cv_string_from_field_attribs(scratch.arena, lf.attribs));
      rd_printf("Value:   %S", cv_string_from_numeric(scratch.arena, value));
    } break;
    case CV_LeafKind_NESTTYPE_ST: 
    case CV_LeafKind_NESTTYPE: {
      CV_LeafNestType lf  = {0};
      String8         name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);
      rd_printf("Name:  %S", name);
      rd_printf("Index: %S", cv_string_from_itype(scratch.arena, lf.itype));
    } break;
    case CV_LeafKind_NOTTRAN: {
      // ms-symbol-pdf: 
      //  This is used when CVPACK encounters a type record that has no equivalent in the Microsoftsymbol information format.
    } break;
    case CV_LeafKind_UDT_SRC_LINE: {
      CV_LeafUDTSrcLine lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Type = %S, Source File = %x, Line = %u", cv_string_from_itype(scratch.arena, lf.udt_itype), lf.src_string_id, lf.line);
    } break;
    case CV_LeafKind_STRING_ID: {
      CV_LeafStringId lf    = {0};
      String8         string = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &string);

      rd_printf("string:     %S", string);
      rd_printf("Substrings: %x", cv_string_from_itemid(arena, lf.substr_list_id)); // TODO: print actual strings instead
    } break;
    case CV_LeafKind_POINTER: {
      CV_LeafPointer lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      CV_PointerKind kind = CV_PointerAttribs_Extract_KIND(lf.attribs);
      CV_PointerMode mode = CV_PointerAttribs_Extract_MODE(lf.attribs);

      rd_printf("Type:    %S", cv_string_from_itype(scratch.arena, lf.itype));
      rd_printf("Attribs: %S", cv_string_from_pointer_attribs(arena, lf.attribs));
      rd_printf("Kind:    %S", cv_string_from_pointer_kind(kind));
      rd_printf("Mode:    %S", cv_string_from_pointer_mode(mode));
      rd_indent();
      if (mode == CV_PointerMode_PtrMem) {
        CV_TypeIndex         itype = 0;
        CV_MemberPointerKind pm    = 0;
        cursor += str8_deserial_read_struct(raw_leaf, cursor, &itype);
        cursor += str8_deserial_read_struct(raw_leaf, cursor, &pm);

        rd_printf("Class Type: %S", cv_string_from_itype(scratch.arena, itype));
        rd_printf("Format:     %S", cv_string_from_member_pointer_kind(pm));
      } else {
        if (kind == CV_PointerKind_BaseSeg) {
          U16 seg;
          cursor += str8_deserial_read_struct(raw_leaf, cursor, &seg);

          rd_printf("Base Segment: %#04x", seg);
        } else if (kind == CV_PointerKind_BaseType) {
          CV_TypeIndex base_itype = 0;
          String8      name       = {0};
          cursor += str8_deserial_read_struct(raw_leaf, cursor, &base_itype);
          cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

          rd_printf("Base Type: %S", cv_string_from_itype(scratch.arena, base_itype));
          rd_printf("Name:      %S", name);
        }
      }
      rd_unindent();
    } break;
    case CV_LeafKind_UNION_ST: 
    case CV_LeafKind_UNION: {
      CV_LeafUnion     lf  = {0};
      CV_NumericParsed num  = {0};
      String8          name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &num);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:        %S",         name);
      rd_printf("Field Count: %u",         lf.count);
      rd_printf("Properties:  %s",         cv_string_from_type_props(scratch.arena, lf.props));
      rd_printf("Field:       %S",         cv_string_from_itype(scratch.arena, lf.field_itype));
      rd_printf("Size:        %S (bytes)", cv_string_from_numeric(scratch.arena, num));
    } break;
    case CV_LeafKind_CLASS_ST: 
    case CV_LeafKind_STRUCTURE_ST: 
    case CV_LeafKind_CLASS:
    case CV_LeafKind_STRUCTURE: {
      CV_LeafStruct    lf  = {0};
      CV_NumericParsed num  = {0};
      String8          name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &num);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:             %S",         name);
      rd_printf("Field Count:      %u",         lf.count);
      rd_printf("Properties:       %S",         cv_string_from_type_props(scratch.arena, lf.props));
      rd_printf("Field List Type:  %S",         cv_string_from_itype(scratch.arena, lf.field_itype));
      rd_printf("Derived Type:     %S",         cv_string_from_itype(scratch.arena, lf.derived_itype));
      rd_printf("VShape:           %S",         cv_string_from_itype(scratch.arena, lf.vshape_itype));
      rd_printf("Size:             %S (bytes)", cv_string_from_numeric(scratch.arena, num));
      if (lf.props & CV_TypeProp_HasUniqueName) {
        String8 unique_name = {0};
        cursor += str8_deserial_read_cstr(raw_leaf, cursor, &unique_name);
        rd_printf("Unique Name:      %S", unique_name);
      }
    } break;
    case CV_LeafKind_SUBSTR_LIST:
    case CV_LeafKind_ARGLIST: {
      CV_LeafArgList lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Types %u", lf.count);
      rd_indent();
      for (U32 i = 0; i < lf.count; ++i) {
        U32 itype = 0;
        cursor += str8_deserial_read_struct(raw_leaf, cursor, &itype);
        rd_printf("%S", cv_string_from_itype(scratch.arena, itype));
      }
      rd_unindent();
    } break;
    case CV_LeafKind_PROCEDURE: {
      CV_LeafProcedure lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      String8 call_kind    = cv_string_from_call_kind(lf.call_kind);
      String8 func_attribs = cv_string_from_function_attribs(scratch.arena, lf.attribs);

      rd_printf("Return type:        %S", cv_string_from_itype(scratch.arena, lf.ret_itype));
      rd_printf("Call Convention:    %S", call_kind);
      rd_printf("Function Attribs:   %S", func_attribs);
      rd_printf("Argumnet Count:     %u", lf.arg_count);
      rd_printf("Argument List Type: %S", cv_string_from_itype(scratch.arena, lf.arg_itype));
    } break;
    case CV_LeafKind_FUNC_ID: {
      CV_LeafFuncId lf  = {0};
      String8       name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:        %S", name);
      rd_printf("Scope Type:  %S", cv_string_from_itype(scratch.arena, lf.scope_string_id));
      rd_printf("Type:        %S", cv_string_from_itype(scratch.arena, lf.itype));
    } break;
    case CV_LeafKind_MODIFIER: {
      CV_LeafModifier lf = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);

      rd_printf("Type:  %S", cv_string_from_itype(scratch.arena, lf.itype));
      rd_printf("Flags: %S", cv_string_from_modifier_flags(scratch.arena, lf.flags));
    } break;
    case CV_LeafKind_ARRAY_ST: 
    case CV_LeafKind_ARRAY: {
      CV_LeafArray     lf  = {0};
      CV_NumericParsed num  = {0};
      String8          name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &num);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Entry type: %S", cv_string_from_itype(scratch.arena, lf.entry_itype));
      rd_printf("Index type: %S", cv_string_from_itype(scratch.arena, lf.index_itype));
      rd_printf("Length:     %S", cv_string_from_numeric(scratch.arena, num));
      rd_printf("Name:       %S", name);
    } break;
    case CV_LeafKind_FIELDLIST: {
      for (U64 idx = 0; cursor < raw_leaf.size;) {
        U16 member_type = 0;
        cursor += str8_deserial_read_struct(raw_leaf, cursor, &member_type);
        String8 raw_member = str8_skip(raw_leaf, cursor);

        rd_printf("list[%u] = %S", idx++, cv_string_from_leaf_name(arena, member_type));
        rd_indent();
        cursor += cv_format_leaf(arena, out, indent, member_type, raw_member);
        cursor  = AlignPow2(cursor, 4);
        rd_unindent();
      }
    } break;
    case CV_LeafKind_MEMBER_ST:
    case CV_LeafKind_MEMBER: {
      CV_LeafMember    lf  = {0};
      CV_NumericParsed num  = {0};
      String8          name = {0};
      cursor += str8_deserial_read_struct(raw_leaf, cursor, &lf);
      cursor += cv_read_numeric(raw_leaf, cursor, &num);
      cursor += str8_deserial_read_cstr(raw_leaf, cursor, &name);

      rd_printf("Name:    %S", name);
      rd_printf("Attribs: %S", cv_string_from_field_attribs(scratch.arena, lf.attribs));
      rd_printf("Type:    %S", cv_string_from_itype(scratch.arena, lf.itype));
      rd_printf("Offset:  %S", cv_string_from_numeric(scratch.arena, num));
    } break;
    // 16bit
    case CV_LeafKind_OEM_16t: 
    case CV_LeafKind_MODIFIER_16t: 
    case CV_LeafKind_POINTER_16t: 
    case CV_LeafKind_ARRAY_16t: 
    case CV_LeafKind_CLASS_16t: 
    case CV_LeafKind_STRUCTURE_16t: 
    case CV_LeafKind_UNION_16t: 
    case CV_LeafKind_ENUM_16t: 
    case CV_LeafKind_PROCEDURE_16t: 
    case CV_LeafKind_MFUNCTION_16t: 
    case CV_LeafKind_COBOL0_16t: 
    case CV_LeafKind_BARRAY_16t: 
    case CV_LeafKind_DIMARRAY_16t: 
    case CV_LeafKind_VFTPATH_16t: 
    case CV_LeafKind_PRECOMP_16t: 
    case CV_LeafKind_ARGLIST_16t: 
    case CV_LeafKind_DEFARG_16t: 
    case CV_LeafKind_FIELDLIST_16t: 
    case CV_LeafKind_DERIVED_16t: 
    case CV_LeafKind_BITFIELD_16t: 
    case CV_LeafKind_METHODLIST_16t: 
    case CV_LeafKind_DIMCONU_16t: 
    case CV_LeafKind_DIMCONLU_16t: 
    case CV_LeafKind_DIMVARU_16t: 
    case CV_LeafKind_DIMVARLU_16t: 
    case CV_LeafKind_BCLASS_16t: 
    case CV_LeafKind_VBCLASS_16t: 
    case CV_LeafKind_IVBCLASS_16t: 
    case CV_LeafKind_ENUMERATE_ST: 
    case CV_LeafKind_FRIENDFCN_16t: 
    case CV_LeafKind_INDEX_16t: 
    case CV_LeafKind_MEMBER_16t: 
    case CV_LeafKind_STMEMBER_16t: 
    case CV_LeafKind_METHOD_16t: 
    case CV_LeafKind_NESTTYPE_16t: 
    case CV_LeafKind_VFUNCTAB_16t: 
    case CV_LeafKind_FRIENDCLS_16t: 
    case CV_LeafKind_ONEMETHOD_16t: 
    case CV_LeafKind_VFUNCOFF_16t: 
    case CV_LeafKind_ST_MAX: 
    // HLSL
    case CV_LeafKind_HLSL: 
    // COBOL
    case CV_LeafKind_COBOL0: 
    case CV_LeafKind_COBOL1: 
    // Manged
    case CV_LeafKind_MANAGED_ST: 
    // undefined
    case CV_LeafKind_LABEL: 
    case CV_LeafKind_ENDPRECOMP: 
    case CV_LeafKind_LIST: 
    case CV_LeafKind_REFSYM: 
    case CV_LeafKind_BARRAY: 
    case CV_LeafKind_DIMARRAY_ST: 
    case CV_LeafKind_VFTPATH: 
    case CV_LeafKind_OEM: 
    case CV_LeafKind_ALIAS_ST: 
    case CV_LeafKind_OEM2: 
    case CV_LeafKind_DEFARG_ST: 
    case CV_LeafKind_DERIVED: 
    case CV_LeafKind_DIMCONU: 
    case CV_LeafKind_DIMCONLU: 
    case CV_LeafKind_DIMVARU: 
    case CV_LeafKind_DIMVARLU: 
    case CV_LeafKind_FRIENDFCN_ST: 
    case CV_LeafKind_INDEX: 
    case CV_LeafKind_FRIENDCLS: 
    case CV_LeafKind_VFUNCOFF: 
    case CV_LeafKind_MEMBERMODIFY_ST: 
    case CV_LeafKind_TYPESERVER_ST: 
    case CV_LeafKind_TYPESERVER: 
    case CV_LeafKind_DIMARRAY: 
    case CV_LeafKind_ALIAS: 
    case CV_LeafKind_DEFARG: 
    case CV_LeafKind_FRIENDFCN: 
    case CV_LeafKind_NESTTYPEEX: 
    case CV_LeafKind_MEMBERMODIFY: 
    case CV_LeafKind_MANAGED: 
    case CV_LeafKind_STRIDED_ARRAY: 
    case CV_LeafKind_MODIFIER_EX: 
    case CV_LeafKind_INTERFACE: 
    case CV_LeafKind_BINTERFACE: 
    case CV_LeafKind_VECTOR: 
    case CV_LeafKind_MATRIX: 
    case CV_LeafKind_VFTABLE: 
    case CV_LeafKind_UDT_MOD_SRC_LINE: {
      rd_errorf("TODO: %#x", kind);
    } break;
  }
  scratch_end(scratch);
  return cursor;
}

internal void
cv_format_debug_t(Arena *arena, String8List *out, String8 indent, CV_DebugT debug_t)
{
  Temp scratch = scratch_begin(&arena, 1);
  for (U64 lf_idx = 0; lf_idx < debug_t.count; ++lf_idx) {
    CV_Leaf lf     = cv_debug_t_get_leaf(debug_t, lf_idx);
    U64     offset = (U64)(lf.data.str-debug_t.v[0]);
    rd_printf("%S (%#x) [%04x-%04x)", cv_string_from_leaf_kind(lf.kind), offset, offset+lf.data.size);
    rd_indent();
    coff_format_leaf(arena, out, indent, lf.kind, lf.data);
    rd_unindent();
  }
  scratch_end(scratch);
}

internal void
cv_format_symbols_c13(Arena *arena, String8List *out, String8 indent, String8 raw_data)
{
  CV_Arch arch = ~0u;

  for (U64 cursor = 0; cursor < raw_data.size; ) {
    CV_SymbolHeader header = {0};
    cursor += str8_deserial_read_struct(raw_data, cursor, &header);

    if (header.kind == CV_SymKind_COMPILE) {
      if (header.size >= sizeof(CV_SymCompile)) {
        CV_SymCompile *comp = str8_deserial_get_raw_ptr(raw_data, cursor, sizeof(*comp));
        arch = comp->machine;
      } else {
        rd_printf("not enough bytes to read S_COMPILE");
      }
    } else if (header.kind == CV_SymKind_COMPILE2) {
      if (header.size >= sizeof(CV_SymCompile2)) {
        CV_SymCompile2 *comp = str8_deserial_get_raw_ptr(raw_data, cursor, sizeof(*comp));
        arch = comp->machine;
      } else {
        rd_printf("not enough bytes to read S_COMPILE2");
      }
    } else if (header.kind == CV_SymKind_COMPILE3) {
      if (header.size >= sizeof(CV_SymCompile3)) {
        CV_SymCompile3 *comp = str8_deserial_get_raw_ptr(raw_data, cursor, sizeof(*comp));
        arch = comp->machine;
      } else {
        rd_printf("not enough bytes to read S_COMPILE3");
      }
    }

    if (header.size >= sizeof(header.kind)) {
      U64     symbol_end = cursor + (header.size - sizeof(header.kind));
      String8 raw_symbol = str8_substr(raw_data, rng_1u64(cursor, symbol_end));

      rd_printf("%S [%04x-%04x)", cv_string_from_sym_kinds(header.kind), cursor, header.size-sizeof(header.size));
      rd_indent();
      cv_format_symbol(arena, out, indent, arch, header.kind, raw_symbol);
      rd_unindent();

      cursor = symbol_end;
    } else {
      rd_errorf("symbol must be at least two bytes long");
    }
  }
}

internal void 
cv_format_lines_c13(Arena *arena, String8List *out, String8 indent, String8 raw_lines)
{
  Temp scratch = scratch_begin(&arena, 1);

  U64 cursor = 0;

  CV_C13SubSecLinesHeader header = {0};
  cursor += str8_deserial_read_struct(raw_lines, cursor, &header);

  B32 has_columns = !!(header.flags & CV_C13SubSecLinesFlag_HasColumns);
  if (has_columns) {
    rd_errorf("TOOD: columns");
  }

  rd_printf("%04x:%08x-%08x, flags = %04x", header.sec, header.sec_off, header.len, header.flags);

  for (; cursor < raw_lines.size; ) {
    CV_C13File file = {0};
    cursor += str8_deserial_read_struct(raw_lines, cursor, &file);

    rd_printf("file = %08x, line count = %u, block size %08x", file.file_off, file.num_lines, file.block_size);

    Temp        temp    = temp_begin(scratch.arena);
    String8List columns = {0};
    for (U32 line_idx = 0; line_idx < file.num_lines; ++line_idx) {
      CV_C13Line line = {0};
      cursor += str8_deserial_read_struct(raw_lines, cursor, &line);

      B32 always_step_in_line_number = line.off == 0xFEEFEE;
      B32 never_step_in_line_number  = line.off == 0xF00F00;

      U32 ln = CV_C13LineFlags_ExtractLineNumber(line.flags);
      //U32 delta   = CV_C13LineFlags_ExtractDeltaToEnd(line.flags);
      //B32 is_stmt = CV_C13LineFlags_ExtractStatement(line.flags);

      if (always_step_in_line_number || never_step_in_line_number) {
        str8_list_pushf(temp.arena, &columns, "%x %08X", ln, line.off);
      } else {
        str8_list_pushf(temp.arena, &columns, "%5u %08X", ln, line.off);
      }

      if ((line_idx+1) % 4 == 0 || (line_idx+1) == file.num_lines) {
        String8 line_str = str8_list_join(scratch.arena, &columns, &(StringJoin){.sep=str8_lit("\t")});
        rd_printf("%S", line_str);

        temp_end(temp);
        temp = temp_begin(scratch.arena);
        MemoryZeroStruct(&columns);
      }
    }
    temp_end(temp);

    if (cursor < raw_lines.size) {
      rd_newline();
    }
  }

  scratch_end(scratch);
}

internal void
cv_format_file_checksums(Arena *arena, String8List *out, String8 indent, String8 raw_chksums)
{
  Temp scratch = scratch_begin(&arena, 1);

  rd_printf("%8s %8s %8s %16s", "File", "Size", "Type", "Chksum");
  for (U64 cursor = 0; cursor < raw_chksums.size; ) {
    CV_C13Checksum chksum = {0};
    cursor += str8_deserial_read_struct(raw_chksums, cursor, &chksum);
    cursor = AlignPow2(cursor, CV_FileCheckSumsAlign);

    Temp     temp       = temp_begin(scratch.arena);
    String8  chksum_str = str8_lit("???");
    U8      *chksum_ptr = str8_deserial_get_raw_ptr(raw_chksums, cursor, chksum.len);
    if (chksum_ptr) {
      chksum_str = rd_format_hex_array(temp.arena, chksum_ptr, chksum.len);
    }

    rd_printf("%08x %08x %8S %S", 
              chksum.name_off, 
              chksum.len, 
              cv_string_from_c13_checksum_kind(chksum.kind),
              chksum_str);

    temp_end(temp);
  }

  scratch_end(scratch);
}

internal void
cv_format_string_table(Arena *arena, String8List *out, String8 indent, String8 raw_strtab)
{
  for (U64 cursor = 0; cursor < raw_strtab.size; ) {
    String8 str = {0};
    cursor += str8_deserial_read_cstr(raw_strtab, cursor, &str);
    rd_printf("%08x %S", cursor, str);
  }
}

internal void
cv_format_inlinee_lines(Arena *arena, String8List *out, String8 indent, String8 raw_data)
{
  Temp scratch = scratch_begin(&arena, 1);

  U64 cursor = 0;

  U32 inlinee_sig = ~0u;
  cursor += str8_deserial_read_struct(raw_data, cursor, &inlinee_sig);

  switch (inlinee_sig) {
    case CV_C13InlineeLinesSig_NORMAL: {
      rd_printf("%-8s %-8s %-8s", "Inlinee", "File ID", "Base LN");
      for (; cursor < raw_data.size; ) {
        CV_C13InlineeSourceLineHeader line = {0};
        cursor += str8_deserial_read_struct(raw_data, cursor, &line);
        rd_printf("%08x %08x %8u", line.inlinee, line.file_off, line.first_source_ln);
      }
    
     } break;
    case CV_C13InlineeLinesSig_EXTRA_FILES: {
      rd_printf("%-8s %-8s %-8s %s", "Inlinee", "File ID", "Base LN", "Extra FileIDs");
      for (; cursor < raw_data.size; ) {
        Temp temp = temp_begin(scratch.arena);
        
        CV_C13InlineeSourceLineHeader line             = {0};
        U32                           extra_file_count = 0;
        cursor += str8_deserial_read_struct(raw_data, cursor, &line);
        cursor += str8_deserial_read_struct(raw_data, cursor, &extra_file_count);

        String8List extra_files_list = {0};
        for (U32 i = 0; i < extra_file_count; ++i) {
          U32 file_id = 0;
          cursor += str8_deserial_read_struct(raw_data, cursor, &file_id);
          str8_list_pushf(temp.arena, &extra_files_list, "%08x", file_id);
        }
        String8 extra_files = str8_list_join(temp.arena, &extra_files_list, &(StringJoin){.sep=str8_lit(" ,")});

        rd_printf("%08x %08x %u %S", line.inlinee, line.file_off, line.first_source_ln, extra_files);

        temp_end(temp);
      }
    } break;
  }

  scratch_end(scratch);
}

internal void
cv_format_symbols_section(Arena *arena, String8List *out, String8 indent, String8 raw_ss)
{
  Temp scratch = scratch_begin(&arena, 1);

  U64 cursor = 0;
  U32 cv_sig = 0;
  cursor += str8_deserial_read_struct(raw_ss, cursor, &cv_sig);

  for (; cursor < raw_ss.size; ) {
    U64                    sst_offset = 0;
    CV_C13SubSectionHeader ss_header  = {0};
    switch (cv_sig) {
      case CV_Signature_C6: {
        rd_printf("TODO: C6");
      } break;

      case CV_Signature_C7: {
        rd_printf("TODO: C7");
      } break;

      case CV_Signature_C11: {
        ss_header.kind = CV_C13SubSectionKind_Symbols;
        ss_header.size = raw_ss.size - sizeof(cv_sig);

        rd_printf("# CodeView C11");
        rd_newline();
      } break;
      case CV_Signature_C13: {
        sst_offset = cursor;
        cursor += str8_deserial_read_struct(raw_ss, cursor, &ss_header);

        coff_pritnf("# CodeView C13");
        rd_newline();
      } break;
    }

    U64     sst_end = cursor + ss_header.size;
    String8 raw_sst = str8_substr(raw_ss, rng_1u64(cursor, sst_end));
    cursor = AlignPow2(sst_end, CV_C13SubSectionAlign);

    rd_printf("# %S [%llx-%llx)", cv_string_from_c13_subsection_kind(ss_header.kind), sst_offset, sst_end);
    rd_indent();
    switch (ss_header.kind) {
      case CV_C13SubSectionKind_Symbols: {
          cv_format_symbols_c13(arena, out, indent, raw_sst);
      } break;
      case CV_C13SubSectionKind_Lines: {
        cv_format_lines_c13(arena, out, indent, raw_sst);
      } break;
      case CV_C13SubSectionKind_FileChksms: {
        cv_format_file_checksums(arena, out, indent, raw_sst);
      } break;
      case CV_C13SubSectionKind_StringTable: {
        cv_format_string_table(arena, out, indent, raw_sst);
      } break;
      case CV_C13SubSectionKind_InlineeLines: {
        cv_format_inlinee_lines(arena, out, indent, raw_sst);
      } break;
      case CV_C13SubSectionKind_FrameData: 
      case CV_C13SubSectionKind_CrossScopeImports:
      case CV_C13SubSectionKind_CrossScopeExports:
      case CV_C13SubSectionKind_IlLines:
      case CV_C13SubSectionKind_FuncMDTokenMap:
      case CV_C13SubSectionKind_TypeMDTokenMap:
      case CV_C13SubSectionKind_MergedAssemblyInput:
      case CV_C13SubSectionKind_CoffSymbolRVA:
      case CV_C13SubSectionKind_XfgHashType:
      case CV_C13SubSectionKind_XfgHashVirtual:
      default: {
        rd_printf("TODO");
      } break;
    }
    rd_unindent();
  }

  scratch_end(scratch);
}

//- COFF

internal void
coff_format_archive_member_header(Arena *arena, String8List *out, String8 indent, COFF_ArchiveMemberHeader header, String8 long_names)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8 time_stamp = coff_string_from_time_stamp(scratch.arena, header.time_stamp);

  rd_printf("Name:       %S"             , header.name    );
  rd_printf("Time Stamp: %S"             , time_stamp     );
  rd_printf("User ID:    %u"             , header.user_id );
  rd_printf("Group ID:   %u"             , header.group_id);
  rd_printf("Mode:       %S"             , header.mode    );
  rd_printf("Data:       [%#llx-%#llx)", header.data_range.min, header.data_range.max);

  scratch_end(scratch);
}

internal void
coff_format_section_table(Arena              *arena,
                          String8List        *out,
                          String8             indent,
                          String8             raw_data,
                          U64                 string_table_off,
                          COFF_Symbol32Array  symbols,
                          U64                 sect_count,
                          COFF_SectionHeader *sect_headers)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 *symlinks = push_array(scratch.arena, String8, sect_count);
  for (U64 i = 0; i < symbols.count; ++i) {
    COFF_Symbol32              *symbol = symbols.v+i;
    COFF_SymbolValueInterpType  interp = coff_interp_symbol(symbol->section_number, symbol->value, symbol->storage_class);
    if (interp == COFF_SymbolValueInterp_REGULAR &&
        symbol->aux_symbol_count == 0 &&
        (symbol->storage_class == COFF_SymStorageClass_EXTERNAL || symbol->storage_class == COFF_SymStorageClass_STATIC)) {
      if (symbol->section_number > 0 && symbol->section_number <= symbols.count) {
        COFF_SectionHeader *header = sect_headers+(symbol->section_number-1);
        if (header->flags & COFF_SectionFlag_LNK_COMDAT) {
          symlinks[symbol->section_number-1] = coff_read_symbol_name(raw_data, string_table_off, &symbol->name);
        }
      }
    }
    i += symbol->aux_symbol_count;
  }

  if (sect_count) {
    rd_printf("# Section Table");
    rd_indent();

    rd_printf("%-4s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-5s %-10s %s",
                "No.",
                "Name",
                "VirtSize",
                "VirtOff",
                "FileSize",
                "FileOff",
                "RelocOff",
                "LinesOff",
                "RelocCnt",
                "LineCnt",
                "Align",
                "Flags",
                "Symlink");

    for (U64 i = 0; i < sect_count; ++i) {
      COFF_SectionHeader *header = sect_headers+i;

      String8 name      = str8_cstring_capped(header->name, header->name+sizeof(header->name));
      String8 full_name = coff_name_from_section_header(header, raw_data, string_table_off);

      String8 align;
      {
        U64 align_size = coff_align_size_from_section_flags(header->flags);
        align = push_str8f(scratch.arena, "%u", align_size);
      }

      String8 flags;
      {
        String8List mem_flags = {0};
        if (header->flags & COFF_SectionFlag_MEM_READ) {
          str8_list_pushf(scratch.arena, &mem_flags, "r");
        }
        if (header->flags & COFF_SectionFlag_MEM_WRITE) {
          str8_list_pushf(scratch.arena, &mem_flags, "w");
        }
        if (header->flags & COFF_SectionFlag_MEM_EXECUTE) {
          str8_list_pushf(scratch.arena, &mem_flags, "x");
        }

        String8List cnt_flags = {0};
        if (header->flags & COFF_SectionFlag_CNT_CODE) {
          str8_list_pushf(scratch.arena, &cnt_flags, "c");
        }
        if (header->flags & COFF_SectionFlag_CNT_INITIALIZED_DATA) {
          str8_list_pushf(scratch.arena, &cnt_flags, "d");
        }
        if (header->flags & COFF_SectionFlag_CNT_UNINITIALIZED_DATA) {
          str8_list_pushf(scratch.arena, &cnt_flags, "u");
        }

        String8List mem_extra_flags = {0};
        if (header->flags & COFF_SectionFlag_MEM_SHARED) {
          str8_list_pushf(scratch.arena, &mem_flags, "s");
        }
        if (header->flags & COFF_SectionFlag_MEM_16BIT) {
          str8_list_pushf(scratch.arena, &mem_extra_flags, "h");
        }
        if (header->flags & COFF_SectionFlag_MEM_LOCKED) {
          str8_list_pushf(scratch.arena, &mem_extra_flags, "l");
        }
        if (header->flags & COFF_SectionFlag_MEM_DISCARDABLE) {
          str8_list_pushf(scratch.arena, &mem_extra_flags, "d");
        }
        if (header->flags & COFF_SectionFlag_MEM_NOT_CACHED) {
          str8_list_pushf(scratch.arena, &mem_extra_flags, "c");
        }
        if (header->flags & COFF_SectionFlag_MEM_NOT_PAGED) {
          str8_list_pushf(scratch.arena, &mem_extra_flags, "p");
        }

        String8List lnk_flags = {0};
        if (header->flags & COFF_SectionFlag_LNK_REMOVE) {
          str8_list_pushf(scratch.arena, &lnk_flags, "r");
        }
        if (header->flags & COFF_SectionFlag_LNK_COMDAT) {
          str8_list_pushf(scratch.arena, &lnk_flags, "c");
        }
        if (header->flags & COFF_SectionFlag_LNK_OTHER) {
          str8_list_pushf(scratch.arena, &lnk_flags, "o");
        }
        if (header->flags & COFF_SectionFlag_LNK_INFO) {
          str8_list_pushf(scratch.arena, &lnk_flags, "i");
        }
        if (header->flags & COFF_SectionFlag_LNK_NRELOC_OVFL) {
          str8_list_pushf(scratch.arena, &lnk_flags, "f");
        }

        String8List other_flags = {0};
        if (header->flags & COFF_SectionFlag_TYPE_NO_PAD) {
          str8_list_pushf(scratch.arena, &other_flags, "n");
        }
        if (header->flags & COFF_SectionFlag_GPREL) {
          str8_list_pushf(scratch.arena, &other_flags, "g");
        }

        String8 mem = str8_list_join(scratch.arena, &mem_flags, 0);
        String8 cnt = str8_list_join(scratch.arena, &cnt_flags, 0);
        String8 lnk = str8_list_join(scratch.arena, &lnk_flags, 0);
        String8 ext = str8_list_join(scratch.arena, &mem_extra_flags, 0);
        String8 oth = str8_list_join(scratch.arena, &other_flags, 0);

        String8List f = {0};
        str8_list_push(scratch.arena, &f, mem);
        str8_list_push(scratch.arena, &f, cnt);
        str8_list_push(scratch.arena, &f, ext);
        str8_list_push(scratch.arena, &f, lnk);
        str8_list_push(scratch.arena, &f, oth);

        flags = str8_list_join(scratch.arena, &f, &(StringJoin){ .sep = str8_lit("-") });

        if (!flags.size) {
          flags = str8_lit("none");
        }
      }

      String8List l = {0};
      str8_list_pushf(scratch.arena, &l, "%-4x",  i+1                );
      str8_list_pushf(scratch.arena, &l, "%-8S",  name               );
      str8_list_pushf(scratch.arena, &l, "%08x",  header->vsize      );
      str8_list_pushf(scratch.arena, &l, "%08x",  header->voff       );
      str8_list_pushf(scratch.arena, &l, "%08x",  header->fsize      );
      str8_list_pushf(scratch.arena, &l, "%08x",  header->foff       );
      str8_list_pushf(scratch.arena, &l, "%08x",  header->relocs_foff);
      str8_list_pushf(scratch.arena, &l, "%08x",  header->lines_foff );
      str8_list_pushf(scratch.arena, &l, "%08x",  header->reloc_count);
      str8_list_pushf(scratch.arena, &l, "%08x",  header->line_count );
      str8_list_pushf(scratch.arena, &l, "%-5S",  align              );
      str8_list_pushf(scratch.arena, &l, "%-10S", flags              );
      if (symlinks[i].size > 0) {
        str8_list_pushf(scratch.arena, &l, "%S", symlinks[i]);
      } else {
        str8_list_pushf(scratch.arena, &l, "[no symlink]");
      }

      String8 line = str8_list_join(scratch.arena, &l, &(StringJoin){ .sep = str8_lit(" "), });
      rd_printf("%S", line);

      if (full_name.size != name.size) {
        rd_indent();
        rd_printf("Full Name: %S", full_name);
        rd_unindent();
      }
    }

    rd_newline();
    rd_printf("Flags:");
    rd_indent();
    rd_printf("r = MEM_READ    w = MEM_WRITE        x = MEM_EXECUTE");
    rd_printf("c = CNT_CODE    d = INITIALIZED_DATA u = UNINITIALIZED_DATA");
    rd_printf("s = MEM_SHARED  h = MEM_16BIT        l = MEM_LOCKED          d = MEM_DISCARDABLE c = MEM_NOT_CACHED  p = MEM_NOT_PAGED");
    rd_printf("r = LNK_REMOVE  c = LNK_COMDAT       o = LNK_OTHER           i = LNK_INFO        f = LNK_NRELOC_OVFL");
    rd_printf("g = GPREL       n = TYPE_NO_PAD");
    rd_unindent();

    rd_unindent();
    rd_newline();
  }

  scratch_end(scratch);
}

internal void
coff_disasm_sections(Arena              *arena,
                     String8List        *out,
                     String8             indent,
                     String8             raw_data,
                     COFF_MachineType    machine,
                     U64                 image_base,
                     B32                 is_obj,
                     RD_MarkerArray        *section_markers,
                     U64                 section_count,
                     COFF_SectionHeader *sections)
{
  if (section_count) {
    for (U64 sect_idx = 0; sect_idx < section_count; ++sect_idx) {
      COFF_SectionHeader *sect = sections+sect_idx;
      if (sect->flags & COFF_SectionFlag_CNT_CODE) {
        U64         sect_off  = is_obj ? sect->foff : sect->voff;
        U64         sect_size = is_obj ? sect->fsize : sect->vsize;
        String8     raw_code  = str8_substr(raw_data, rng_1u64(sect->foff, sect->foff+sect_size));
        RD_MarkerArray markers   = section_markers[sect_idx];

        rd_printf("# Disassembly [Section No. %#llx]", (sect_idx+1));
        rd_indent();
        rd_format_disasm(arena, out, indent, machine, image_base, sect_off, markers.count, markers.v, raw_code);
        rd_unindent();
      }
    }
  }
}

internal void
coff_raw_data_sections(Arena              *arena,
                       String8List        *out,
                       String8             indent,
                       String8             raw_data,
                       B32                 is_obj,
                       RD_MarkerArray        *section_markers,
                       U64                 section_count,
                       COFF_SectionHeader *sections)
{
  if (section_count) {
    for (U64 sect_idx = 0; sect_idx < section_count; ++sect_idx) {
      COFF_SectionHeader *sect = sections+sect_idx;
      if (sect->fsize > 0) {
        U64         sect_size = is_obj ? sect->fsize : sect->vsize;
        String8     raw_sect  = str8_substr(raw_data, rng_1u64(sect->foff, sect->foff+sect_size));
        RD_MarkerArray markers   = section_markers[sect_idx];

        rd_printf("# Raw Data [Section No. %#llx]", (sect_idx+1));
        rd_indent();
        rd_format_raw_data(arena, out, indent, 32, markers.count, markers.v, raw_sect);
        rd_unindent();
        rd_newline();
      }
    }
  }
}

internal void
coff_format_relocs(Arena              *arena,
                   String8List        *out,
                   String8             indent,
                   String8             raw_data,
                   U64                 string_table_off,
                   COFF_MachineType    machine,
                   U64                 sect_count,
                   COFF_SectionHeader *sect_headers,
                   COFF_Symbol32Array  symbols)
{
  Temp scratch = scratch_begin(&arena, 1);

  B32 print_header = 1;

  for (U64 sect_idx = 0; sect_idx < sect_count; ++sect_idx) {
    COFF_SectionHeader *sect_header = sect_headers+sect_idx;
    COFF_RelocInfo      reloc_info  = coff_reloc_info_from_section_header(raw_data, sect_header);

    if (reloc_info.count) {
      if (print_header) {
        print_header = 0;
        rd_printf("# Relocations");
        rd_indent();
      }

      rd_printf("## Section %llx", sect_idx);
      rd_indent();

      rd_printf("%-4s %-8s %-16s %-16s %-8s %-7s", "No.", "Offset", "Type", "ApplyTo", "SymIdx", "SymName");

      for (U64 reloc_idx = 0; reloc_idx < reloc_info.count; ++reloc_idx) {
        COFF_Reloc *reloc      = (COFF_Reloc*)(raw_data.str + reloc_info.array_off) + reloc_idx;
        String8     type       = coff_string_from_reloc(machine, reloc->type);
        U64         apply_size = coff_apply_size_from_reloc(machine, reloc->type);

        U64 apply_foff = sect_header->foff + reloc->apply_off;
        if (apply_foff + apply_size > raw_data.size) {
          rd_errorf("out of bounds apply file offset %#llx in relocation %#llx", apply_foff, reloc_idx);
          break;
        }

        U64 raw_apply;
        AssertAlways(apply_size <= sizeof(raw_apply));
        MemoryCopy(&raw_apply, raw_data.str + apply_foff, apply_size);
        S64 apply = extend_sign64(raw_apply, apply_size);

        if (reloc->isymbol > symbols.count) {
          rd_errorf("out of bounds symbol index %u in relocation %#llx", reloc->isymbol, reloc_idx);
          break;
        }

        COFF_Symbol32 *symbol      = symbols.v+reloc->isymbol;
        String8        symbol_name = coff_read_symbol_name(raw_data, string_table_off, &symbol->name);

        String8List line = {0};
        str8_list_pushf(scratch.arena, &line, "%-4x",  reloc_idx       );
        str8_list_pushf(scratch.arena, &line, "%08x",  reloc->apply_off);
        str8_list_pushf(scratch.arena, &line, "%-16S", type            );
        str8_list_pushf(scratch.arena, &line, "%016x", apply           );
        str8_list_pushf(scratch.arena, &line, "%S",    symbol_name     );

        String8 l = str8_list_join(scratch.arena, &line, &(StringJoin){.sep=str8_lit(" ")});
        rd_printf("%S", l);
      }

      rd_unindent();
    }
  }

  if (!print_header) {
    rd_unindent();
  }
  rd_newline();

  scratch_end(scratch);
}

internal void
coff_format_symbol_table(Arena              *arena,
                         String8List        *out,
                         String8             indent,
                         String8             raw_data,
                         B32                 is_big_obj,
                         U64                 string_table_off,
                         COFF_Symbol32Array  symbols)
{
  Temp scratch = scratch_begin(&arena, 1);

  if (symbols.count) {
    rd_printf("# Symbol Table");
    rd_indent();

    rd_printf("%-4s %-8s %-10s %-4s %-4s %-4s %-16s %-20s", 
                "No.", "Value", "SectNum", "Aux", "Msb", "Lsb", "Storage", "Name");

    for (U64 i = 0; i < symbols.count; ++i) {
      COFF_Symbol32 *symbol        = &symbols.v[i];
      String8        name          = coff_read_symbol_name(raw_data, string_table_off, &symbol->name);
      String8        msb           = coff_string_from_sym_dtype(symbol->type.u.msb);
      String8        lsb           = coff_string_from_sym_type(symbol->type.u.lsb);
      String8        storage_class = coff_string_from_sym_storage_class(symbol->storage_class);
      String8        section_number;
      switch (symbol->section_number) {
        case COFF_SYMBOL_UNDEFINED_SECTION: section_number = str8_lit("UNDEF"); break;
        case COFF_SYMBOL_ABS_SECTION:       section_number = str8_lit("ABS");   break;
        case COFF_SYMBOL_DEBUG_SECTION:     section_number = str8_lit("DEBUG"); break;
        default:                            section_number = push_str8f(scratch.arena, "%010x", symbol->section_number); break;
      }

      String8List line = {0};
      str8_list_pushf(scratch.arena, &line, "%-4x",  i                       );
      str8_list_pushf(scratch.arena, &line, "%08x",  symbol->value           );
      str8_list_pushf(scratch.arena, &line, "%-10S", section_number          );
      str8_list_pushf(scratch.arena, &line, "%-4u",  symbol->aux_symbol_count);
      str8_list_pushf(scratch.arena, &line, "%-4S",  msb                     );
      str8_list_pushf(scratch.arena, &line, "%-4S",  lsb                     );
      str8_list_pushf(scratch.arena, &line, "%-16S", storage_class           );
      str8_list_pushf(scratch.arena, &line, "%S",    name                    );

      String8 l = str8_list_join(scratch.arena, &line, &(StringJoin){.sep = str8_lit(" ")});
      rd_printf("%S", l);

      rd_indent();
      for (U64 k=i+1, c = i+symbol->aux_symbol_count; k <= c; ++k) {
        void *raw_aux = &symbols.v[k];
        switch (symbol->storage_class) {
          case COFF_SymStorageClass_EXTERNAL: {
            COFF_SymbolFuncDef *func_def = (COFF_SymbolFuncDef*)&symbols.v[k];
            rd_printf("Tag Index %#x, Total Size %#x, Line Numbers %#x, Next Function %#x", 
                        func_def->tag_index, func_def->total_size, func_def->ptr_to_ln, func_def->ptr_to_next_func);
          } break;
          case COFF_SymStorageClass_FUNCTION: {
            COFF_SymbolFunc *func = raw_aux;
            rd_printf("Ordinal Line Number %#x, Next Function %#x", func->ln, func->ptr_to_next_func);
          } break;
          case COFF_SymStorageClass_WEAK_EXTERNAL: {
            COFF_SymbolWeakExt *weak = raw_aux;
            String8             type = coff_string_from_weak_ext_type(weak->characteristics);
            rd_printf("Tag Index %#x, Characteristics %S", weak->tag_index, type);
          } break;
          case COFF_SymStorageClass_FILE: {
            COFF_SymbolFile *file = raw_aux;
            String8          name = str8_cstring_capped(file->name, file->name+sizeof(file->name));
            rd_printf("Name %S", name);
          } break;
          case COFF_SymStorageClass_STATIC: {
            COFF_SymbolSecDef *sd        = raw_aux;
            String8            selection = coff_string_from_selection(sd->selection);
            U32 number = sd->number_lo;
            if (is_big_obj) {
              number |= (U32)sd->number_hi << 16;
            }
            if (number) {
              rd_printf("Length %x, Reloc Count %u, Line Count %u, Checksum %x, Section %x, Selection %S",
                          sd->length, sd->number_of_relocations, sd->number_of_ln, sd->check_sum, number, selection);
            } else {
              rd_printf("Length %x, Reloc Count %u, Line Count %u, Checksum %x",
                          sd->length, sd->number_of_relocations, sd->number_of_ln, sd->check_sum);
            }
          } break;
          default: {
            rd_printf("???");
          } break;
        }
      }

      i += symbol->aux_symbol_count;
      rd_unindent();
    }

    rd_unindent();
    rd_newline();
  }

  scratch_end(scratch);
}

internal void
coff_format_big_obj_header(Arena *arena, String8List *out, String8 indent, COFF_HeaderBigObj *header)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 time_stamp = coff_string_from_time_stamp(scratch.arena, header->time_stamp);
  String8 machine    = coff_string_from_machine_type(header->machine);

  rd_printf("# Big Obj");
  rd_indent();

  rd_printf("Time Stamp:    %S",     time_stamp             );
  rd_printf("Machine:       %S",        machine             );
  rd_printf("Section Count: %u",  header->section_count     );
  rd_printf("Symbol Table:  %#x", header->symbol_table_foff);
  rd_printf("Symbol Count:  %u",   header->symbol_count     );

  rd_unindent();

  scratch_end(scratch);
}

internal void
coff_format_header(Arena *arena, String8List *out, String8 indent, COFF_Header *header)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 time_stamp = coff_string_from_time_stamp(scratch.arena, header->time_stamp);
  String8 machine    = coff_string_from_machine_type(header->machine);
  String8 flags      = coff_string_from_flags(scratch.arena, header->flags);

  rd_printf("# COFF Header");
  rd_indent();
  rd_printf("Time Stamp:           %S",   time_stamp                  );
  rd_printf("Machine:              %S",   machine                     );
  rd_printf("Section Count:        %u",   header->section_count       );
  rd_printf("Symbol Table:         %#x", header->symbol_table_foff   );
  rd_printf("Symbol Count:         %u",   header->symbol_count        );
  rd_printf("Optional Header Size: %m",   header->optional_header_size);
  rd_printf("Flags:                %S",   flags                       );
  rd_unindent();

  scratch_end(scratch);
}

internal void
coff_format_import(Arena *arena, String8List *out, String8 indent, COFF_ImportHeader *header)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 machine    = coff_string_from_machine_type(header->machine);
  String8 time_stamp = coff_string_from_time_stamp(scratch.arena, header->time_stamp);

  rd_printf("# Import");
  rd_indent();
  rd_printf("Version:    %u", header->version  );
  rd_printf("Machine:    %S", machine          );
  rd_printf("Time Stamp: %S", time_stamp       );
  rd_printf("Data Size:  %m", header->data_size);
  rd_printf("Hint:       %u", header->hint     );
  rd_printf("Type:       %u", header->type     );
  rd_printf("Name Type:  %u", header->name_type);
  rd_printf("Function:   %S", header->func_name);
  rd_printf("DLL:        %S", header->dll_name );
  rd_unindent();

  scratch_end(scratch);
}

internal void
coff_format_big_obj(Arena *arena, String8List *out, String8 indent, String8 raw_data, RD_Option opts)
{
  Temp scratch = scratch_begin(&arena, 1);

  COFF_HeaderBigObj  *big_obj          = str8_deserial_get_raw_ptr(raw_data, 0, sizeof(COFF_HeaderBigObj));
  COFF_SectionHeader *sections         = str8_deserial_get_raw_ptr(raw_data, sizeof(COFF_HeaderBigObj), sizeof(COFF_SectionHeader)*big_obj->section_count);
  U64                 string_table_off = big_obj->symbol_table_foff + sizeof(COFF_Symbol32)*big_obj->symbol_count;
  COFF_Symbol32Array  symbols          = coff_symbol_array_from_data_32(scratch.arena, raw_data, big_obj->symbol_table_foff, big_obj->symbol_count);

  if (opts & RD_Option_Headers) {
    coff_format_big_obj_header(arena, out, indent, big_obj);
    rd_newline();
  }

  if (opts & RD_Option_Sections) {
    Rng1U64 sect_headers_range = rng_1u64(sizeof(*big_obj), sizeof(*big_obj) + sizeof(COFF_SectionHeader)*big_obj->section_count);
    Rng1U64 symbols_range      = rng_1u64(big_obj->symbol_table_foff, big_obj->symbol_table_foff + sizeof(COFF_Symbol32)*big_obj->symbol_count);

    if (sect_headers_range.max > raw_data.size) {
      rd_errorf("not enough bytes to read big obj section headers");
      goto exit;
    }
    if (big_obj->symbol_count) {
      if (symbols_range.max > raw_data.size) {
        rd_errorf("not enough bytes to read big obj symbol table");
        goto exit;
      }
      if (contains_1u64(symbols_range, sect_headers_range.min) ||
        contains_1u64(symbols_range, sect_headers_range.max)) {
        rd_errorf("section headers and symbol table ranges overlap");
        goto exit;
      }
    }

    coff_format_section_table(arena, out, indent, raw_data, string_table_off, symbols, big_obj->section_count, sections);
    rd_newline();
  }

  if (opts & RD_Option_Relocs) {
    coff_format_relocs(arena, out, indent, raw_data, string_table_off, big_obj->machine, big_obj->section_count, sections, symbols);
    rd_newline();
  }

  if (opts & RD_Option_Symbols) {
    coff_format_symbol_table(arena, out, indent, raw_data, string_table_off, 1, symbols);
    rd_newline();
  }

exit:;
  scratch_end(scratch);
}

internal void
coff_format_obj(Arena *arena, String8List *out, String8 indent, String8 raw_data, RD_Option opts)
{
  Temp scratch = scratch_begin(&arena, 1);

  COFF_Header        *header           = (COFF_Header *)raw_data.str;
  COFF_SectionHeader *sections         = (COFF_SectionHeader *)(header+1);
  U64                 string_table_off = header->symbol_table_foff + sizeof(COFF_Symbol16)*header->symbol_count;
  COFF_Symbol32Array  symbols          = coff_symbol_array_from_data_16(scratch.arena, raw_data, header->symbol_table_foff, header->symbol_count);

  if (opts & RD_Option_Headers) {
    coff_format_header(arena, out, indent, header);
    rd_newline();
  }

  if (opts & RD_Option_Sections) {
    Rng1U64 sect_headers_range = rng_1u64(sizeof(*header), sizeof(*header) + sizeof(COFF_SectionHeader)*header->section_count);
    Rng1U64 symbols_range      = rng_1u64(header->symbol_table_foff, header->symbol_table_foff + sizeof(COFF_Symbol16)*header->symbol_count);

    if (sect_headers_range.max > raw_data.size) {
      rd_errorf("not enough bytes to read obj section headers");
      goto exit;
    }
    if (header->symbol_count) {
      if (symbols_range.max > raw_data.size) {
        rd_errorf("not enough bytes to read obj symbol table");
        goto exit;
      }
      if (contains_1u64(symbols_range, sect_headers_range.min) ||
        contains_1u64(symbols_range, sect_headers_range.max)) {
        rd_errorf("section headers and symbol table ranges overlap");
        goto exit;
      }
    }

    coff_format_section_table(arena, out, indent, raw_data, string_table_off, symbols, header->section_count, sections);
    rd_newline();
  }

  if (opts & RD_Option_Relocs) {
    coff_format_relocs(arena, out, indent, raw_data, string_table_off, header->machine, header->section_count, sections, symbols);
    rd_newline();
  }

  if (opts & RD_Option_Symbols) {
    coff_format_symbol_table(arena, out, indent, raw_data, 0, string_table_off, symbols);
    rd_newline();
  }

  RD_MarkerArray *section_markers = 0;
  if (opts & (RD_Option_Disasm|RD_Option_Rawdata)) {
    section_markers = rd_section_markers_from_coff_symbol_table(scratch.arena, raw_data, string_table_off, header->section_count, symbols);
  }

  if (opts & RD_Option_Rawdata) {
    coff_raw_data_sections(arena, out, indent, raw_data, 1, section_markers, header->section_count, sections);
  }

  if (opts & RD_Option_Disasm) {
    coff_disasm_sections(arena, out, indent, raw_data, header->machine, 0, 1, section_markers, header->section_count, sections);
    rd_newline();
  }

exit:;
  scratch_end(scratch);
}

internal void
coff_format_archive(Arena *arena, String8List *out, String8 indent, String8 raw_archive, RD_Option opts)
{
  Temp scratch = scratch_begin(&arena, 1);

  COFF_ArchiveParse archive_parse = coff_archive_parse_from_data(raw_archive);

  if (archive_parse.error.size) {
    rd_errorf("%S", archive_parse.error);
    return;
  }

  COFF_ArchiveFirstMember first_member = archive_parse.first_member;
  {
    rd_printf("# First Header");
    rd_indent();

    rd_printf("Symbol Count:      %u", first_member.symbol_count);
    rd_printf("String Table Size: %M", first_member.string_table.size);

    rd_printf("Members:");
    rd_indent();

    String8List string_table = str8_split_by_string_chars(scratch.arena, first_member.string_table, str8_lit("\0"), 0);

    if (string_table.node_count == first_member.member_offset_count) {
      String8Node *string_n = string_table.first;

      for (U64 i = 0; i < string_table.node_count; ++i, string_n = string_n->next) {
        U32 offset = from_be_u32(first_member.member_offsets[i]);
        rd_printf("[%4u] %#08x %S", i, offset, string_n->string);
      }
    } else {
      rd_errorf("Member offset count (%llu) doesn't match string table count (%llu)", first_member.member_offset_count);
    }

    rd_unindent();
    rd_unindent();
    rd_newline();
  }

  if (archive_parse.has_second_header) {
    COFF_ArchiveSecondMember second_member = archive_parse.second_member;

    rd_printf("# Second Header");
    rd_indent();

    rd_printf("Member Count:      %u", second_member.member_count);
    rd_printf("Symbol Count:      %u", second_member.symbol_count);
    rd_printf("String Table Size: %M", second_member.string_table.size);

    String8List string_table = str8_split_by_string_chars(scratch.arena, second_member.string_table, str8_lit("\0"), 0);

    rd_printf("Members:");
    rd_indent();
    if (second_member.symbol_index_count == second_member.symbol_count) {
      String8Node *string_n = string_table.first;
      for (U64 i = 0; i < second_member.symbol_count; ++i, string_n = string_n->next) {
        U16 symbol_number = second_member.symbol_indices[i];
        if (symbol_number > 0 && symbol_number <= second_member.member_offset_count) {
          U16 symbol_idx    = symbol_number - 1;
          U32 member_offset = second_member.member_offsets[i];
          rd_printf("[%4u] %#08x %S", i, member_offset, string_n->string);
        } else {
          rd_errorf("[%4u] Out of bounds symbol number %u", i, symbol_number);
          break;
        }
      }
    } else {
      rd_errorf("Symbol index count %u doesn't match symbol count %u",
                           second_member.symbol_index_count, second_member.symbol_count);
    }
    rd_unindent();

    rd_unindent();
    rd_newline();
  }

  if (archive_parse.has_long_names && opts & RD_Option_LongNames) {
    rd_printf("# Long Names");
    rd_indent();

    String8List long_names = str8_split_by_string_chars(scratch.arena, archive_parse.long_names, str8_lit("\0"), 0);
    U64 name_idx = 0;
    for (String8Node *name_n = long_names.first; name_n != 0; name_n = name_n->next, ++name_idx) {
      U64 offset = (U64)(name_n->string.str - archive_parse.long_names.str);
      rd_printf("[%-4u] %#08x %S", name_idx, offset, name_n->string);
    }

    rd_unindent();
    rd_newline();
  }

  U64  member_offset_count = 0;
  U32 *member_offsets      = 0;
  if (archive_parse.has_second_header) {
    member_offset_count = archive_parse.second_member.member_offset_count;
    member_offsets      = archive_parse.second_member.member_offsets;
  } else {
    HashTable *ht = hash_table_init(scratch.arena, 0x1000);
    for (U64 i = 0; i < archive_parse.first_member.member_offset_count; ++i) {
      U32 member_offset = from_be_u32(archive_parse.first_member.member_offsets[i]);
      if (!hash_table_search_u32(ht, member_offset)) {
        hash_table_push_u32_raw(scratch.arena, ht, member_offset, 0);
      }
    }
    member_offset_count = ht->count;
    member_offsets      = keys_from_hash_table_u32(scratch.arena, ht);
    radsort(member_offsets, member_offset_count, u32_is_before);
  }

  rd_printf("# Members");
  rd_indent();

  for (U64 i = 0; i < member_offset_count; ++i) {
    U64                next_member_offset = i+1 < member_offset_count ? member_offsets[i+1] : raw_archive.size;
    U64                member_offset      = member_offsets[i];
    String8            raw_member         = str8_substr(raw_archive, rng_1u64(member_offset, next_member_offset));
    COFF_ArchiveMember member             = coff_archive_member_from_data(raw_member);
    COFF_DataType      member_type        = coff_data_type_from_data(member.data);

    rd_printf("Member @ %#llx", member_offset);
    rd_indent();

    if (opts & RD_Option_Headers) {
      coff_format_archive_member_header(arena, out, indent, member.header, archive_parse.long_names);
      rd_newline();
    }

    switch (member_type) {
      case COFF_DataType_BIG_OBJ: {
        coff_format_big_obj(arena, out, indent, member.data, opts);
      } break;
      case COFF_DataType_OBJ: {
        coff_format_obj(arena, out, indent, member.data, opts);
      } break;
      case COFF_DataType_IMPORT: {
        if (opts & RD_Option_Headers) {
          COFF_ImportHeader header = {0};
          U64 parse_size = coff_parse_archive_import(member.data, 0, &header);
          if (parse_size) {
            coff_format_import(arena, out, indent, &header);
          } else {
            rd_errorf("not enough bytes to parse import header");
          }
        }
      } break;
      case COFF_DataType_NULL: {
        rd_errorf("unknown member format", member_offset);
      } break;
    }

    rd_unindent();
    rd_newline();
  }

  rd_unindent();

  scratch_end(scratch);
}

//- MSVC CRT

internal void
mscrt_format_eh_handler_type32(Arena *arena, String8List *out, String8 indent, MSCRT_EhHandlerType32 *handler)
{
  String8 catch_line     = str8_zero(); // TODO: syms_line_for_voff(scratch.arena, group, handler->catch_handler_voff);
  String8 adjectives_str = mscrt_string_from_eh_adjectives(arena, handler->adjectives);
  rd_printf("Adjectives:                %S",     adjectives_str, handler->adjectives);
  rd_printf("Descriptor:                %#x",    handler->descriptor_voff);
  rd_printf("Catch Object Frame Offset: %#x",    handler->catch_obj_frame_offset);
  rd_printf("Catch Handler:             %#x %S", handler->catch_handler_voff, catch_line);
  rd_printf("Delta to FP Handler:       %#x",    handler->fp_distance);
}

////////////////////////////////
//~ PE

internal void
pe_format_data_directory_ranges(Arena *arena, String8List *out, String8 indent, U64 count, PE_DataDirectory *dirs)
{
  Temp scratch = scratch_begin(&arena, 1);
  rd_printf("# Data Directories");
  rd_indent();
  for (U64 i = 0; i < count; ++i) {
    String8 dir_name;
    if (i < PE_DataDirectoryIndex_COUNT) {
      dir_name = pe_string_from_data_directory_index(i);
    } else {
      dir_name = push_str8f(scratch.arena, "%#x", i);
    }
    rd_printf("%-16S [%08x-%08x)", dir_name, dirs[i].virt_off, dirs[i].virt_off+dirs[i].virt_size);
  }
  rd_unindent();
  scratch_end(scratch);
}

internal void
pe_format_optional_header32(Arena *arena, String8List *out, String8 indent, PE_OptionalHeader32 *opt_header, PE_DataDirectory *dirs)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8 subsystem = pe_string_from_subsystem(opt_header->subsystem);
  String8 dll_chars = pe_string_from_dll_characteristics(scratch.arena, opt_header->dll_characteristics);

  rd_printf("# PE Optional Header 32");
  rd_indent();
  rd_printf("Magic:                 %#x",   opt_header->magic);
  rd_printf("Linker version:        %u.%u", opt_header->major_linker_version, opt_header->minor_linker_version);
  rd_printf("Size of code:          %m",    opt_header->sizeof_code);
  rd_printf("Size of inited data:   %m",    opt_header->sizeof_inited_data);
  rd_printf("Size of uninited data: %m",    opt_header->sizeof_uninited_data);
  rd_printf("Entry point:           %#x",   opt_header->entry_point_va);
  rd_printf("Code base:             %#x",   opt_header->code_base);
  rd_printf("Data base:             %#x",   opt_header->data_base);
  rd_printf("Image base:            %#x",   opt_header->image_base);
  rd_printf("Section align:         %#x",   opt_header->section_alignment);
  rd_printf("File align:            %#x",   opt_header->file_alignment);
  rd_printf("OS version:            %u.%u", opt_header->major_os_ver, opt_header->minor_os_ver);
  rd_printf("Image Version:         %u.%u", opt_header->major_img_ver, opt_header->minor_img_ver);
  rd_printf("Subsystem version:     %u.%u", opt_header->major_subsystem_ver, opt_header->minor_subsystem_ver);
  rd_printf("Win32 version:         %u",    opt_header->win32_version_value);
  rd_printf("Size of image:         %m",    opt_header->sizeof_image);
  rd_printf("Size of headers:       %m",    opt_header->sizeof_headers);
  rd_printf("Checksum:              %#x",   opt_header->check_sum);
  rd_printf("Subsystem:             %S",    subsystem);
  rd_printf("DLL Characteristics:   %S",    dll_chars);
  rd_printf("Stack reserve:         %m",    opt_header->sizeof_stack_reserve);
  rd_printf("Stack commit:          %m",    opt_header->sizeof_stack_commit);
  rd_printf("Heap reserve:          %m",    opt_header->sizeof_heap_reserve);
  rd_printf("Heap commit:           %m",    opt_header->sizeof_heap_commit);
  rd_printf("Loader flags:          %#x",   opt_header->loader_flags);
  rd_printf("RVA and offset count:  %u",    opt_header->data_dir_count);
  rd_newline();

  pe_format_data_directory_ranges(arena, out, indent, opt_header->data_dir_count, dirs);
  rd_newline();

  rd_unindent();
  scratch_end(scratch);
}

internal void
pe_format_optional_header32plus(Arena *arena, String8List *out, String8 indent, PE_OptionalHeader32Plus *opt_header, PE_DataDirectory *dirs)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8 subsystem = pe_string_from_subsystem(opt_header->subsystem);
  String8 dll_chars = pe_string_from_dll_characteristics(scratch.arena, opt_header->dll_characteristics);

  rd_printf("# PE Optional Header 32+");
  rd_indent();
  rd_printf("Magic:                 %#x",   opt_header->magic);
  rd_printf("Linker version:        %u.%u", opt_header->major_linker_version, opt_header->minor_linker_version);
  rd_printf("Size of code:          %m",    opt_header->sizeof_code);
  rd_printf("Size of inited data:   %m",    opt_header->sizeof_inited_data);
  rd_printf("Size of uninited data: %m",    opt_header->sizeof_uninited_data);
  rd_printf("Entry point:           %#x",   opt_header->entry_point_va);
  rd_printf("Code base:             %#x",   opt_header->code_base);
  rd_printf("Image base:            %#llx", opt_header->image_base);
  rd_printf("Section align:         %#x",   opt_header->section_alignment);
  rd_printf("File align:            %#x",   opt_header->file_alignment);
  rd_printf("OS version:            %u.%u", opt_header->major_os_ver, opt_header->minor_os_ver);
  rd_printf("Image Version:         %u.%u", opt_header->major_img_ver, opt_header->minor_img_ver);
  rd_printf("Subsystem version:     %u.%u", opt_header->major_subsystem_ver, opt_header->minor_subsystem_ver);
  rd_printf("Win32 version:         %u",    opt_header->win32_version_value);
  rd_printf("Size of image:         %m",    opt_header->sizeof_image);
  rd_printf("Size of headers:       %m",    opt_header->sizeof_headers);
  rd_printf("Checksum:              %#x",   opt_header->check_sum);
  rd_printf("Subsystem:             %S",    subsystem);
  rd_printf("DLL Characteristics:   %S",    dll_chars);
  rd_printf("Stack reserve:         %M",    opt_header->sizeof_stack_reserve);
  rd_printf("Stack commit:          %M",    opt_header->sizeof_stack_commit);
  rd_printf("Heap reserve:          %M",    opt_header->sizeof_heap_reserve);
  rd_printf("Heap commit:           %M",    opt_header->sizeof_heap_commit);
  rd_printf("Loader flags:          %#x",   opt_header->loader_flags);
  rd_printf("RVA and offset count:  %u",    opt_header->data_dir_count);
  rd_newline();

  pe_format_data_directory_ranges(arena, out, indent, opt_header->data_dir_count, dirs);
  rd_newline();

  rd_unindent();
  scratch_end(scratch);
}

internal void
pe_format_load_config32(Arena *arena, String8List *out, String8 indent, PE_LoadConfig32 *lc)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 time_stamp        = coff_string_from_time_stamp(scratch.arena, lc->time_stamp);
  String8 global_flag_clear = pe_string_from_global_flags(scratch.arena, lc->global_flag_clear);
  String8 global_flag_set   = pe_string_from_global_flags(scratch.arena, lc->global_flag_set);

  rd_printf("# Load Config 32");
  rd_indent();

  rd_printf("Size:                          %m",       lc->size);
  rd_printf("Time stamp:                    %#x (%S)", lc->time_stamp, time_stamp);
  rd_printf("Version:                       %u.%u",    lc->major_version, lc->minor_version);
  rd_printf("Global flag clear:             %#x %S",   global_flag_clear);
  rd_printf("Global flag set:               %#x %S",   global_flag_set);
  rd_printf("Critical section timeout:      %u",       lc->critical_section_timeout);
  rd_printf("Decommit free block threshold: %#x",      lc->decommit_free_block_threshold);
  rd_printf("Decommit total free threshold: %#x",      lc->decommit_total_free_threshold);
  rd_printf("Lock prefix table:             %#x",      lc->lock_prefix_table);
  rd_printf("Maximum alloc size:            %m",       lc->maximum_allocation_size);
  rd_printf("Virtual memory threshold:      %m",       lc->virtual_memory_threshold);
  rd_printf("Process affinity mask:         %#x",      lc->process_affinity_mask);
  rd_printf("Process heap flags:            %#x",      lc->process_heap_flags);
  rd_printf("CSD version:                   %u",       lc->csd_version);
  rd_printf("Edit list:                     %#x",      lc->edit_list);
  rd_printf("Security Cookie:               %#x",      lc->security_cookie);
  if (lc->size < OffsetOf(PE_LoadConfig64, seh_handler_table)) {
    goto exit;
  }
  rd_newline();

  rd_printf("SEH Handler Table: %#x", lc->seh_handler_table);
  rd_printf("SEH Handler Count: %u",   lc->seh_handler_count);
  if (lc->size < OffsetOf(PE_LoadConfig64, guard_cf_check_func_ptr)) {
    goto exit;
  }
  rd_newline();

  rd_printf("Guard CF Check Function:    %#x", lc->guard_cf_check_func_ptr);
  rd_printf("Guard CF Dispatch Function: %#x", lc->guard_cf_dispatch_func_ptr);
  rd_printf("Guard CF Function Table:    %#x", lc->guard_cf_func_table);
  rd_printf("Guard CF Function Count:    %u",  lc->guard_cf_func_count);
  rd_printf("Guard Flags:                %#x", lc->guard_flags);
  if (lc->size < OffsetOf(PE_LoadConfig64, code_integrity)) {
    goto exit;
  }
  rd_newline();

  rd_printf("Code integrity:                        { Flags = %#x, Catalog = %#x, Catalog Offset = %#x }",
               lc->code_integrity.flags, lc->code_integrity.catalog, lc->code_integrity.catalog_offset);
  rd_printf("Guard address taken IAT entry table:   %#x", lc->guard_address_taken_iat_entry_table);
  rd_printf("Guard address taken IAT entry count:   %u",  lc->guard_address_taken_iat_entry_count);
  rd_printf("Guard long jump target table:          %#x", lc->guard_long_jump_target_table);
  rd_printf("Guard long jump target count:          %u",  lc->guard_long_jump_target_count);
  rd_printf("Dynamic value reloc table:             %#x", lc->dynamic_value_reloc_table);
  rd_printf("CHPE Metadata ptr:                     %#x", lc->chpe_metadata_ptr);
  rd_printf("Guard RF failure routine:              %#x", lc->guard_rf_failure_routine);
  rd_printf("Guard RF failure routine func ptr:     %#x", lc->guard_rf_failure_routine_func_ptr);
  rd_printf("Dynamic value reloc section:           %#x", lc->dynamic_value_reloc_table_section);
  rd_printf("Dynamic value reloc section offset:    %#x", lc->dynamic_value_reloc_table_offset);
  rd_printf("Guard RF verify SP func ptr:           %#x", lc->guard_rf_verify_stack_pointer_func_ptr);
  rd_printf("Hot patch table offset:                %#x", lc->hot_patch_table_offset);
  if (lc->size < OffsetOf(PE_LoadConfig64, enclave_config_ptr)) {
    goto exit;
  }
  rd_newline();

  rd_printf("Enclave config ptr:                    %#x", lc->enclave_config_ptr);
  rd_printf("Volatile metadata ptr:                 %#x", lc->volatile_metadata_ptr);
  rd_printf("Guard EH continuation table:           %#x", lc->guard_eh_continue_table);
  rd_printf("Guard EH continuation count:           %u",  lc->guard_eh_continue_count);
  rd_printf("Guard XFG check func ptr:              %#x", lc->guard_xfg_check_func_ptr);
  rd_printf("Guard XFG dispatch func ptr:           %#x", lc->guard_xfg_dispatch_func_ptr);
  rd_printf("Guard XFG table dispatch func ptr:     %#x", lc->guard_xfg_table_dispatch_func_ptr);
  rd_printf("Cast guard OS determined failure mode: %#x", lc->cast_guard_os_determined_failure_mode);
  rd_newline();

exit:;
  rd_unindent();
  scratch_end(scratch);
}

internal void
pe_format_load_config64(Arena *arena, String8List *out, String8 indent, PE_LoadConfig64 *lc)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 time_stamp        = coff_string_from_time_stamp(scratch.arena, lc->time_stamp);
  String8 global_flag_clear = pe_string_from_global_flags(scratch.arena, lc->global_flag_clear);
  String8 global_flag_set   = pe_string_from_global_flags(scratch.arena, lc->global_flag_set);

  rd_printf("# Load Config 64");
  rd_indent();

  rd_printf("Size:                          %m",       lc->size);
  rd_printf("Time stamp:                    %#x (%S)", lc->time_stamp, time_stamp);
  rd_printf("Version:                       %u.%u",    lc->major_version, lc->minor_version);
  rd_printf("Global flag clear:             %#x %S",   lc->global_flag_clear, global_flag_clear);
  rd_printf("Global flag set:               %#x %S",   lc->global_flag_set, global_flag_set);
  rd_printf("Critical section timeout:      %u",       lc->critical_section_timeout);
  rd_printf("Decommit free block threshold: %#llx",    lc->decommit_free_block_threshold);
  rd_printf("Decommit total free threshold: %#llx",    lc->decommit_total_free_threshold);
  rd_printf("Lock prefix table:             %#llx",    lc->lock_prefix_table);
  rd_printf("Maximum alloc size:            %M",       lc->maximum_allocation_size);
  rd_printf("Virtual memory threshold:      %M",       lc->virtual_memory_threshold);
  rd_printf("Process affinity mask:         %#x",      lc->process_affinity_mask);
  rd_printf("Process heap flags:            %#x",      lc->process_heap_flags);
  rd_printf("CSD version:                   %u",       lc->csd_version);
  rd_printf("Edit list:                     %#llx",    lc->edit_list);
  rd_printf("Security Cookie:               %#llx",    lc->security_cookie);
  if (lc->size < OffsetOf(PE_LoadConfig64, seh_handler_table)) {
    goto exit;
  }
  rd_newline();

  rd_printf("SEH Handler Table: %#llx", lc->seh_handler_table);
  rd_printf("SEH Handler Count: %llu",  lc->seh_handler_count);
  if (lc->size < OffsetOf(PE_LoadConfig64, guard_cf_check_func_ptr)) {
    goto exit;
  }
  rd_newline();

  rd_printf("Guard CF Check Function:    %#llx", lc->guard_cf_check_func_ptr);
  rd_printf("Guard CF Dispatch Function: %#llx", lc->guard_cf_dispatch_func_ptr);
  rd_printf("Guard CF Function Table:    %#llx", lc->guard_cf_func_table);
  rd_printf("Guard CF Function Count:    %llu",  lc->guard_cf_func_count);
  rd_printf("Guard Flags:                %#x",   lc->guard_flags);
  if (lc->size < OffsetOf(PE_LoadConfig64, code_integrity)) {
    goto exit;
  }
  rd_newline();

  rd_printf("Code integrity:                      { Flags = %#x, Catalog = %#x, Catalog Offset = %#x }",
               lc->code_integrity.flags, lc->code_integrity.catalog, lc->code_integrity.catalog_offset);
  rd_printf("Guard address taken IAT entry table: %#llx", lc->guard_address_taken_iat_entry_table);
  rd_printf("Guard address taken IAT entry count: %llu",  lc->guard_address_taken_iat_entry_count);
  rd_printf("Guard long jump target table:        %#llx", lc->guard_long_jump_target_table);
  rd_printf("Guard long jump target count:        %llu",  lc->guard_long_jump_target_count);
  rd_printf("Dynamic value reloc table:           %#llx", lc->dynamic_value_reloc_table);
  rd_printf("CHPE Metadata ptr:                   %#llx", lc->chpe_metadata_ptr);
  rd_printf("Guard RF failure routine:            %#llx", lc->guard_rf_failure_routine);
  rd_printf("Guard RF failure routine func ptr:   %#llx", lc->guard_rf_failure_routine_func_ptr);
  rd_printf("Dynamic value reloc section:         %#llx", lc->dynamic_value_reloc_table_section);
  rd_printf("Dynamic value reloc section offset:  %#llx", lc->dynamic_value_reloc_table_offset);
  rd_printf("Guard RF verify SP func ptr:         %#llx", lc->guard_rf_verify_stack_pointer_func_ptr);
  rd_printf("Hot patch table offset:              %#llx", lc->hot_patch_table_offset);
  if (lc->size < OffsetOf(PE_LoadConfig64, enclave_config_ptr)) {
    goto exit;
  }
  rd_newline();

  rd_printf("Enclave config ptr:                    %#llx", lc->enclave_config_ptr);
  rd_printf("Volatile metadata ptr:                 %#llx", lc->volatile_metadata_ptr);
  rd_printf("Guard EH continuation table:           %#llx", lc->guard_eh_continue_table);
  rd_printf("Guard EH continuation count:           %llu",  lc->guard_eh_continue_count);
  rd_printf("Guard XFG check func ptr:              %#llx", lc->guard_xfg_check_func_ptr);
  rd_printf("Guard XFG dispatch func ptr:           %#llx", lc->guard_xfg_dispatch_func_ptr);
  rd_printf("Guard XFG table dispatch func ptr:     %#llx", lc->guard_xfg_table_dispatch_func_ptr);
  rd_printf("Cast guard OS determined failure mode: %#llx", lc->cast_guard_os_determined_failure_mode);
  rd_newline();

exit:;
  rd_unindent();
  scratch_end(scratch);
}

internal void
pe_format_tls(Arena *arena, String8List *out, String8 indent, PE_ParsedTLS tls)
{
  Temp scratch = scratch_begin(&arena, 1);

  rd_printf("# TLS");
  rd_indent();

  String8 tls_chars = coff_string_from_section_flags(scratch.arena, tls.header.characteristics);
  rd_printf("Raw data start:    %#llx", tls.header.raw_data_start);
  rd_printf("Raw data end:      %#llx", tls.header.raw_data_end);
  rd_printf("Index address:     %#llx", tls.header.index_address);
  rd_printf("Callbacks address: %#llx", tls.header.callbacks_address);
  rd_printf("Zero-fill size:    %m",    tls.header.zero_fill_size);
  rd_printf("Characteristics:   %S",    tls_chars);

  if (tls.callback_count) {
    rd_newline();
    rd_printf("## Callbacks");
    rd_indent();
    for (U64 i = 0; i < tls.callback_count; ++i) {
      rd_printf("%#llx", tls.callback_addrs[i]);
    }
    rd_unindent();
  }

  rd_unindent();
  rd_newline();

  scratch_end(scratch);
}

internal void
pe_format_debug_directory(Arena *arena, String8List *out, String8 indent, String8 raw_data, String8 raw_dir)
{
  Temp scratch = scratch_begin(&arena, 1);

  rd_printf("# Debug");
  rd_indent();

  U64                entry_count = raw_dir.size / sizeof(PE_DebugDirectory);
  PE_DebugDirectory *entries      = str8_deserial_get_raw_ptr(raw_dir, 0, sizeof(*entries)*entry_count);
  for (U64 i = 0; i < entry_count; ++i) {
    PE_DebugDirectory *de = entries+i;
    if (i > 0) {
      rd_newline();
    }
    rd_printf("Entry[%u]", i);
    rd_indent();

    {
      String8 time_stamp = coff_string_from_time_stamp(scratch.arena, de->time_stamp);
      String8 type       = pe_string_from_debug_directory_type(de->type);

      rd_printf("Characteristics: %#x",   de->characteristics);
      rd_printf("Time Stamp:      %S",    time_stamp);
      rd_printf("Version:         %u.%u", de->major_ver, de->minor_ver);
      rd_printf("Type:            %S",    type);
      rd_printf("Size:            %u",    de->size);
      rd_printf("Data virt off:   %#x",   de->voff);
      rd_printf("Data file off:   %#x",   de->foff);
      rd_newline();
    }

    String8 raw_entry = str8_substr(raw_data, rng_1u64(de->foff, de->foff+de->size));
    if (raw_entry.size != de->size) {
      rd_errorf("unable to read debug entry @ %#x", de->foff);
      break;
    }

    rd_indent();
    switch (de->type) {
      case PE_DebugDirectoryType_ILTCG:
      case PE_DebugDirectoryType_MPX:
      case PE_DebugDirectoryType_EXCEPTION:
      case PE_DebugDirectoryType_FIXUP:
      case PE_DebugDirectoryType_OMAP_TO_SRC:
      case PE_DebugDirectoryType_OMAP_FROM_SRC:
      case PE_DebugDirectoryType_BORLAND:
      case PE_DebugDirectoryType_CLSID:
      case PE_DebugDirectoryType_REPRO:
      case PE_DebugDirectoryType_EX_DLLCHARACTERISTICS: {
        NotImplemented;
      } break;
      case PE_DebugDirectoryType_COFF_GROUP: {
        U64 off = 0;

        // TODO: is this version?
        U32 unknown  = 0;
        off += str8_deserial_read_struct(raw_entry, off, &unknown);
        if (unknown != 0) {
          rd_printf("TODO: unknown: %u", unknown);
        }

        rd_printf("%-8s %-8s %-8s", "VOFF", "Size", "Name");
        for (; off < raw_entry.size; ) {
          U32     voff = 0;
          U32     size = 0;
          String8 name = str8_zero();

          off += str8_deserial_read_struct(raw_entry, off, &voff);
          off += str8_deserial_read_struct(raw_entry, off, &size);
          if (voff == 0 && size == 0) {
            break;
          }
          off += str8_deserial_read_cstr(raw_entry, off, &name);
          off = AlignPow2(off, 4);
		  
          rd_printf("%08x %08x %S", voff, size, name);
        }
      } break;
      case PE_DebugDirectoryType_VC_FEATURE: {
        MSCRT_VCFeatures *feat = str8_deserial_get_raw_ptr(raw_entry, 0, sizeof(*feat));
        if (feat) {
          rd_printf("Pre-VC++ 11.0: %u", feat->pre_vcpp);
          rd_printf("C/C++:         %u", feat->c_cpp);
          rd_printf("/GS:           %u", feat->gs);
          rd_printf("/sdl:          %u", feat->sdl);
          rd_printf("guardN:        %u", feat->guard_n);
        } else {
          rd_errorf("not enough bytes to read VC Features");
        }
      } break;
      case PE_DebugDirectoryType_FPO: {
        PE_DebugFPO *fpo = str8_deserial_get_raw_ptr(raw_entry, 0, sizeof(*fpo));
        if (fpo) {
          U8          prolog_size     = PE_FPOEncoded_Extract_PROLOG_SIZE(fpo->flags);
          U8          saved_regs_size = PE_FPOEncoded_Extract_SAVED_REGS_SIZE(fpo->flags);
          PE_FPOType  type            = PE_FPOEncoded_Extract_FRAME_TYPE(fpo->flags);
          PE_FPOFlags flags           = PE_FPOEncoded_Extract_FLAGS(fpo->flags);

          String8 type_string  = pe_string_from_fpo_type(type);
          String8 flags_string = pe_string_from_fpo_flags(scratch.arena, flags);

          rd_printf("Function offset: %#x", fpo->func_code_off);
          rd_printf("Function size:   %#x", fpo->func_size);
          rd_printf("Locals size:     %u",  fpo->locals_size);
          rd_printf("Params size:     %u",  fpo->params_size);
          rd_printf("Prolog size:     %u",  prolog_size);
          rd_printf("Saved regs size: %u",  saved_regs_size);
          rd_printf("Type:            %S",  type_string);
          rd_printf("Flags:           %S",  flags_string);
        } else {
          rd_errorf("not enough bytes to read FPO");
        }
      } break;

      case PE_DebugDirectoryType_CODEVIEW: {
        U32 magic = 0;
        str8_deserial_read_struct(raw_entry, 0, &magic);
        switch (magic) {
          case PE_CODEVIEW_PDB20_MAGIC: {
            PE_CvHeaderPDB20 *cv20 = str8_deserial_get_raw_ptr(raw_entry, 0, sizeof(*cv20));
            String8 name; str8_deserial_read_cstr(raw_entry, sizeof(*cv20), &name);

            String8 time_stamp = coff_string_from_time_stamp(scratch.arena, cv20->time_stamp);

            rd_printf("Time stamp: %S", time_stamp);
            rd_printf("Age:        %u", cv20->age);
            rd_printf("Name:       %S", name);
          } break;
          case PE_CODEVIEW_PDB70_MAGIC: {
            PE_CvHeaderPDB70 *cv70 = str8_deserial_get_raw_ptr(raw_entry, 0, sizeof(*cv70));
            String8 name; str8_deserial_read_cstr(raw_entry, sizeof(*cv70), &name);
            
            String8 guid = string_from_guid(scratch.arena, cv70->guid);

            rd_printf("GUID: %S", guid);
            rd_printf("Age:  %u", cv70->age);
            rd_printf("Name: %S", name);
          } break;
          default: {
            rd_errorf("unknown CodeView magic %#x", magic);
          } break;
        }
      } break;
      case PE_DebugDirectoryType_MISC: {
        PE_DebugMisc *misc = str8_deserial_get_raw_ptr(raw_entry, 0, sizeof(*misc));
        
        String8 type_string = pe_string_from_misc_type(misc->data_type);

        rd_printf("Data type: %S", type_string);
        rd_printf("Size:      %u", misc->size);
        rd_printf("Unicode:   %u", misc->unicode);

        switch (misc->data_type) {
          case PE_DebugMiscType_EXE_NAME: {
            String8 name;
            str8_deserial_read_cstr(raw_entry, sizeof(*misc), &name);
            rd_printf("Name: %S", name);
          } break;
          default: {
            rd_printf("???");
          } break;
        }
      } break;
    }
    rd_unindent();

    rd_unindent();
  }

  rd_unindent();
  rd_newline();
  scratch_end(scratch);
}

internal void
pe_format_export_table(Arena *arena, String8List *out, String8 indent, PE_ParsedExportTable exptab)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 time_stamp = coff_string_from_time_stamp(scratch.arena, exptab.time_stamp);

  rd_printf("# Export Table");
  rd_indent();

  rd_printf("Characteristics: %u",      exptab.flags);
  rd_printf("Time stamp:      %S",      time_stamp);
  rd_printf("Version:         %u.%02u", exptab.major_ver, exptab.minor_ver);
  rd_printf("Ordinal base:    %u",      exptab.ordinal_base);
  rd_printf("");

  rd_printf("%-4s %-8s %-8s %-8s", "No.", "Oridnal", "VOff", "Name");

  for (U64 i = 0; i < exptab.export_count; ++i) {
    PE_ParsedExport *exp = exptab.exports+i;
    if (exp->forwarder.size) {
      rd_printf("%4u %8u %8x %S (forwarded to %S)", i, exp->ordinal, exp->voff, exp->name, exp->forwarder);
    } else {
      rd_printf("%4u %8u %8x %S", i, exp->ordinal, exp->voff, exp->name);
    }
  }

  rd_unindent();
  scratch_end(scratch);
}

internal void
pe_format_static_import_table(Arena *arena, String8List *out, String8 indent, U64 image_base, PE_ParsedStaticImportTable imptab)
{
  Temp scratch = scratch_begin(&arena, 1);

  if (imptab.count) {
    rd_printf("# Import Table");
    rd_indent();
    for (U64 dll_idx = 0; dll_idx < imptab.count; ++dll_idx) {
      PE_ParsedStaticDLLImport *dll = imptab.v+dll_idx;

      rd_printf("Name:                 %S",    dll->name);
      rd_printf("Import address table: %#llx", image_base + dll->import_address_table_voff);
      rd_printf("Import name table:    %#llx", image_base + dll->import_name_table_voff);
      rd_printf("Time stamp:           %#x",   dll->time_stamp);
      rd_newline();

      if (dll->import_count) {
        rd_indent();
        for (U64 imp_idx = 0; imp_idx < dll->import_count; ++imp_idx) {
          PE_ParsedImport *imp = dll->imports+imp_idx;
          if (imp->type == PE_ParsedImport_Ordinal) {
            rd_printf("%#-6x", imp->u.ordinal);
          } else if (imp->type == PE_ParsedImport_Name) {
            rd_printf("%#-6x %S", imp->u.name.hint, imp->u.name.string);
          }
        }
        rd_unindent();
        rd_newline();
      }
    }
    rd_unindent();
  }

  scratch_end(scratch);
}

internal void
pe_format_delay_import_table(Arena *arena, String8List *out, String8 indent, U64 image_base, PE_ParsedDelayImportTable imptab)
{
  if (imptab.count) {
    Temp scratch = scratch_begin(&arena, 1);
    rd_printf("# Delay Import Table");
    rd_indent();

    for (U64 dll_idx = 0; dll_idx < imptab.count; ++dll_idx) {
      PE_ParsedDelayDLLImport *dll = imptab.v+dll_idx;

      rd_printf("Attributes:               %#08x", dll->attributes);
      rd_printf("Name:                     %S",    dll->name);
      rd_printf("HMODULE address:          %#llx", image_base + dll->module_handle_voff);
      rd_printf("Import address table:     %#llx", image_base + dll->iat_voff);
      rd_printf("Import name table:        %#llx", image_base + dll->name_table_voff);
      rd_printf("Bound import name table:  %#llx", image_base + dll->bound_table_voff);
      rd_printf("Unload import name table: %#llx", image_base + dll->unload_table_voff);
      rd_printf("Time stamp:               %#x",   dll->time_stamp);
      rd_newline();

      rd_indent();
      for (U64 imp_idx = 0; imp_idx < dll->import_count; ++imp_idx) {
        PE_ParsedImport *imp = dll->imports+imp_idx;

        String8 bound = str8_lit("NULL");
        if (imp_idx < dll->bound_table_count) {
          U64 bound_addr = dll->bound_table[imp_idx];
          bound = push_str8f(scratch.arena, "%#llx", bound_addr);
        }

        String8 unload = str8_lit("NULL");
        if (imp_idx < dll->unload_table_count) {
          U64 unload_addr = dll->unload_table[imp_idx];
          unload = push_str8f(scratch.arena, "%#llx", unload_addr);
        }

        if (imp->type == PE_ParsedImport_Ordinal) {
          rd_printf("%-16S %-16S %#-4x", bound, unload, imp->u.ordinal);
        } else if (imp->type == PE_ParsedImport_Name) {
          rd_printf("%-16S %-16S %#-4x %S", bound, unload, imp->u.name.hint, imp->u.name.string);
        }
      }
      rd_unindent();

      rd_newline();
    }

    rd_unindent();
    scratch_end(scratch);
  }
}

internal void
pe_format_resources(Arena *arena, String8List *out, String8 indent, PE_ResourceDir *root)
{
  Temp scratch = scratch_begin(&arena, 1);

  // setup stack
  struct stack_s {
    struct stack_s  *next;
    B32              print_table;
    B32              is_named;
    PE_ResourceNode *curr_name_node;
    PE_ResourceNode *curr_id_node;
    U64              name_idx;
    U64              id_idx;
    U64              dir_idx;
    U64              dir_id;
    String8          dir_name;
    PE_ResourceDir  *table;
  } *stack = push_array(scratch.arena, struct stack_s, 1);
  stack->table          = root;
  stack->print_table    = 1;
  stack->is_named       = 1;
  stack->dir_name       = str8_lit("ROOT");
  stack->curr_name_node = root->named_list.first;
  stack->curr_id_node   = root->id_list.first;

  if (stack) {
    rd_printf("# Resources");

    // traverse resource tree
    while (stack) {
      if (stack->print_table) {
        stack->print_table = 0;
        rd_indent();
        
        if (stack->is_named) {
          rd_printf("[%u] %S { Time Stamp: %u, Version %u.%u Name Count: %u, ID Count %u, Characteristics: %u }", 
                      stack->dir_idx,
                      stack->dir_name,
                      stack->table->time_stamp, 
                      stack->table->major_version, stack->table->minor_version, 
                      stack->table->named_list.count, stack->table->id_list.count,
                      stack->table->characteristics);
        } else {
          B32 is_actually_leaf = stack->table->id_list.count == 1 && 
                                 stack->table->id_list.first->data.kind != PE_ResDataKind_DIR;
          if (is_actually_leaf) {
            rd_printf("[%u] %u { Time Stamp: %u, Version %u.%u Name Count: %u, ID Count %u, Characteristics: %u }", 
                        stack->dir_idx,
                        stack->dir_id,
                        stack->table->time_stamp, 
                        stack->table->major_version, stack->table->minor_version, 
                        stack->table->named_list.count, stack->table->id_list.count,
                        stack->table->characteristics);
          } else {
            String8 id_str = pe_resource_kind_to_string(stack->dir_id);
            rd_printf("[%u] %S { Time Stamp: %u, Version %u.%u Name Count: %u, ID Count %u, Characteristics: %u }", 
                        stack->dir_idx,
                        id_str,
                        stack->dir_id,
                        stack->table->time_stamp, 
                        stack->table->major_version, stack->table->minor_version, 
                        stack->table->named_list.count, stack->table->id_list.count,
                        stack->table->characteristics);
          }
        }
      }
      
      while (stack->curr_name_node) {
        PE_ResourceNode *named_node = stack->curr_name_node;
        stack->curr_name_node = stack->curr_name_node->next;
        U64 name_idx = stack->name_idx++;
        
        PE_Resource *res = &named_node->data;
        if (res->kind == PE_ResDataKind_DIR) {
          struct stack_s *frame = push_array(scratch.arena, struct stack_s, 1);
          frame->table          = res->u.dir;
          frame->print_table    = 1;
          frame->dir_idx        = stack->name_idx;
          frame->dir_name       = res->id.u.string;
          frame->is_named       = 1;
          frame->curr_name_node = frame->table->named_list.first;
          frame->curr_id_node   = frame->table->id_list.first;
          SLLStackPush(stack, frame);
          goto yield;
        } else if (res->kind == PE_ResDataKind_COFF_LEAF) {
          COFF_ResourceDataEntry *entry = &res->u.leaf;
          rd_printf("[%u] %S Data VOFF: %#08x, Data Size: %#08x, Code Page: %u", 
                      name_idx, res->id.u.string, entry->data_voff, entry->data_size, entry->code_page);
        } else {
          InvalidPath;
        }
      }

      while (stack->curr_id_node) {
        PE_ResourceNode *id_node = stack->curr_id_node;
        PE_Resource     *res     = &id_node->data;
        stack->curr_id_node = stack->curr_id_node->next;
        U64 id_idx = stack->id_idx++;
        
        if (res->kind == PE_ResDataKind_DIR) {
          struct stack_s *frame = push_array(scratch.arena, struct stack_s, 1);
          frame->table          = res->u.dir;
          frame->print_table    = 1;
          frame->dir_idx        = stack->table->named_list.count + id_idx;
          frame->dir_id         = res->id.u.number;
          frame->curr_name_node = frame->table->named_list.first;
          frame->curr_id_node   = frame->table->id_list.first;
          SLLStackPush(stack, frame);
          goto yield;
        } else if (res->kind == PE_ResDataKind_COFF_LEAF) {
          COFF_ResourceDataEntry *entry = &res->u.leaf;
          rd_printf("[%u] ID: %u Data VOFF: %#08x, Data Size: %#08x, Code Page: %u", id_idx, res->id.u.number, entry->data_voff, entry->data_size, entry->code_page);
        } else {
          InvalidPath;
        }
      }

      if (stack->curr_id_node == 0 && stack->curr_name_node == 0) {
        rd_unindent();
      }

      SLLStackPop(stack);
      
      yield:;
    }

    rd_newline();
  }

  scratch_end(scratch);
}

internal void
pe_format_exceptions_x8664(Arena              *arena,
                           String8List        *out,
                           String8             indent,
                           U64                 section_count,
                           COFF_SectionHeader *sections,
                           String8             raw_data,
                           Rng1U64             except_frange)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8 raw_except = str8_substr(raw_data, except_frange);
  U64     count      = raw_except.size / sizeof(PE_IntelPdata);
  for (U64 i = 0; i < count; ++i) {
    Temp temp = temp_begin(scratch.arena);

    U64            pdata_offset = i * sizeof(PE_IntelPdata);
    PE_IntelPdata *pdata        = str8_deserial_get_raw_ptr(raw_except, pdata_offset, sizeof(*pdata));

    String8 pdata_name = str8_zero(); // TODO: syms_name_for_voff(arena, group, thunk_accel, thunk_table_set, pdata.voff_first);

    U64            unwind_info_offset = coff_foff_from_voff(sections, section_count, pdata->voff_unwind_info);
    PE_UnwindInfo *uwinfo             = str8_deserial_get_raw_ptr(raw_data, unwind_info_offset, sizeof(*uwinfo));

    U8 version        = PE_UNWIND_INFO_VERSION_FROM_HDR(uwinfo->header);
    U8 flags          = PE_UNWIND_INFO_FLAGS_FROM_HDR(uwinfo->header);
    U8 frame_register = PE_UNWIND_INFO_REG_FROM_FRAME(uwinfo->frame);
    U8 frame_offset   = PE_UNWIND_INFO_OFF_FROM_FRAME(uwinfo->frame);

    B32 is_chained       = (flags & PE_UnwindInfoFlag_CHAINED) != 0;
    B32 has_handler_data = !is_chained && (flags & (PE_UnwindInfoFlag_EHANDLER | PE_UnwindInfoFlag_UHANDLER)) != 0;

    String8 flags_str = str8_zero();
    {
      U64 f = flags;

      String8List flags_list = {0};
      if (f & PE_UnwindInfoFlag_EHANDLER) {
        f &= ~PE_UnwindInfoFlag_EHANDLER;
        str8_list_pushf(scratch.arena, &flags_list, "EHANDLER");
      }
      if (f & PE_UnwindInfoFlag_UHANDLER) {
        f &= ~PE_UnwindInfoFlag_UHANDLER;
        str8_list_pushf(scratch.arena, &flags_list, "UHANDLER");
      }
      if (f & PE_UnwindInfoFlag_CHAINED) {
        f &= ~PE_UnwindInfoFlag_CHAINED;
        str8_list_pushf(scratch.arena, &flags_list, "CHAINED");
      }
      if (f) {
        str8_list_pushf(scratch.arena, &flags_list, "%#llx", f);
      }
      if (flags_list.node_count == 0) {
        str8_list_pushf(scratch.arena, &flags_list, "%#llx", f);
      }
      flags_str = str8_list_join(scratch.arena, &flags_list, &(StringJoin){.sep=str8_lit(", ")});
    }

    U64            codes_offset = unwind_info_offset + sizeof(PE_UnwindInfo);
    PE_UnwindCode *code_ptr     = str8_deserial_get_raw_ptr(raw_data, codes_offset, sizeof(*code_ptr) * uwinfo->codes_num);
    PE_UnwindCode *code_opl     = code_ptr + uwinfo->codes_num;

    if (i > 0) {
      rd_newline();
    }
    rd_printf("%08x %08x %08x %08x%s%S",
                pdata_offset,
                pdata->voff_first,
                pdata->voff_one_past_last,
                pdata->voff_unwind_info,
                pdata_name.size ? " " : "", pdata_name);
    rd_printf("Version:     %u",  version);
    rd_printf("Flags:       %S",  flags_str);
    rd_printf("Prolog Size: %#x", uwinfo->prolog_size);
    rd_printf("Code Count:  %u",  uwinfo->codes_num);
    rd_printf("Frame:       %u",  uwinfo->frame);
    rd_printf("Codes:");
    rd_indent();
    for (; code_ptr < code_opl;) {
      Temp code_temp = temp_begin(scratch.arena);
      String8List code_list = {0};

      U8 operation_code = PE_UNWIND_OPCODE_FROM_FLAGS(code_ptr[0].flags);
      U8 operation_info = PE_UNWIND_INFO_FROM_FLAGS(code_ptr[0].flags);

      str8_list_pushf(code_temp.arena, &code_list, "%#04x:", code_ptr[0].off_in_prolog);
      switch (operation_code) {
        case PE_UnwindOpCode_PUSH_NONVOL: {
          String8 gpr = pe_string_from_unwind_gpr_x64(operation_info);
          str8_list_pushf(code_temp.arena, &code_list, "PUSH_NONVOL %S", gpr);
          code_ptr += 1;
        } break;
        case PE_UnwindOpCode_ALLOC_LARGE: {
          U64 size = 0;
          switch (operation_info) {
            case 0: { // 136B - 512K
              size = code_ptr[1].u16*8;
            } break;
            case 1: { // 512K - 4GB
              size = code_ptr[1].u16 + ((U32)code_ptr[2].u16 << 16);
            } break;
            default: break;
          }
          str8_list_pushf(code_temp.arena, &code_list, "ALLOC_LARGE size=%#x", size);
          code_ptr += 2;
        } break;
        case PE_UnwindOpCode_ALLOC_SMALL: {
          U64 size = operation_info*8 + 8;
          str8_list_pushf(code_temp.arena, &code_list, "ALLOC_SMALL size=%#x", size);
          code_ptr += 1;
        } break;
        case PE_UnwindOpCode_SET_FPREG: {
          U64     offset = frame_offset*16;
          String8 gpr    = pe_string_from_unwind_gpr_x64(frame_register);
          str8_list_pushf(code_temp.arena, &code_list, "SET_FPREG %S, offset=%#x", gpr, offset);
          code_ptr += 1;
        } break;
        case PE_UnwindOpCode_SAVE_NONVOL: {
          String8 gpr             = pe_string_from_unwind_gpr_x64(operation_info);
          U64     register_offset = code_ptr[1].u16*8;
          str8_list_pushf(code_temp.arena, &code_list, "SAVE_NONVOL %S, offset=%#x", gpr, register_offset);
          code_ptr += 2;
        } break;
        case PE_UnwindOpCode_SAVE_NONVOL_FAR: {
          String8 gpr          = pe_string_from_unwind_gpr_x64(operation_info);
          U64     frame_offset = code_ptr[1].u16 + ((U32)code_ptr[2].u16 << 16);
          str8_list_pushf(code_temp.arena, &code_list, "SAVE_NONVOL_FAR %S, offset=%#x", gpr, frame_offset);
          code_ptr += 3;
        } break;
        case PE_UnwindOpCode_EPILOG: {
          str8_list_pushf(code_temp.arena, &code_list, "EPILOG flags=%#x", code_ptr[0].flags);
          code_ptr += 1;
        } break;
        case PE_UnwindOpCode_SPARE_CODE: {
          str8_list_pushf(code_temp.arena, &code_list, "SPARE_CODE");
          code_ptr += 1;
        } break;
        case PE_UnwindOpCode_SAVE_XMM128: {
          String8 gpr             = pe_string_from_unwind_gpr_x64(operation_info);
          U64     register_offset = code_ptr[1].u16*16;
          str8_list_pushf(code_temp.arena, &code_list, "SAVE_XMM128 %S, offset=%#x", gpr, register_offset);
          code_ptr += 2;
        } break;
        case PE_UnwindOpCode_SAVE_XMM128_FAR: {
          String8 gpr          = pe_string_from_unwind_gpr_x64(operation_info);
          U64     frame_offset = code_ptr[1].u16 + ((U32)code_ptr[2].u16 << 16);
          str8_list_pushf(code_temp.arena, &code_list, "SAVE_XMM128_FAR %S, offset=%#x", gpr, frame_offset);
          code_ptr += 3;
        } break;
        case PE_UnwindOpCode_PUSH_MACHFRAME: {
          str8_list_pushf(code_temp.arena, &code_list, "PUSH_MACHFRAME %s", operation_info == 1  ? "with error code" : "without error code");
          code_ptr += 1;
        } break;
        default: {
          str8_list_pushf(code_temp.arena, &code_list, "UNKNOWN_OPCODE %#x", operation_code);
          code_ptr += 1;
        } break;
      }

      String8 code_line = str8_list_join(code_temp.arena, &code_list, &(StringJoin){.sep=str8_lit(" ")});
      rd_printf("%S", code_line);

      temp_end(code_temp);
    }
    rd_unindent();

    if (is_chained) {
      U64            next_pdata_offset = codes_offset + sizeof(PE_UnwindCode) * AlignPow2(uwinfo->codes_num, 2);
      PE_IntelPdata *next_pdata        = str8_deserial_get_raw_ptr(raw_data, next_pdata_offset, sizeof(*next_pdata));
      rd_printf("Chained: %#08x %#08x %#08x", next_pdata->voff_first, next_pdata->voff_one_past_last, next_pdata->voff_unwind_info);
    }

    if (has_handler_data) {
#define ExceptionHandlerDataFlag_FuncInfo   (1 << 0)
#define ExceptionHandlerDataFlag_FuncInfo4  (1 << 1)
#define ExceptionHandlerDataFlag_ScopeTable (1 << 2)
#define ExceptionHandlerDataFlag_GS         (1 << 3u)

      U64 actual_code_count = PE_UNWIND_INFO_GET_CODE_COUNT(uwinfo->codes_num);
      U64 read_cursor       = codes_offset + actual_code_count * sizeof(PE_UnwindCode);

      U32 handler = 0; 
      read_cursor += str8_deserial_read_struct(raw_data, read_cursor, &handler);

      String8 handler_name = str8_zero(); // TODO: syms_name_for_voff(scratch.arena, group, thunk_accel, thunk_table_set, handler);

      rd_printf("Handler: %#llx%s%S", handler, handler_name.size ? " " : "", handler_name);

      U32 handler_data_flags = 0;
      if (str8_match(handler_name, str8_lit("__GSHandlerCheck_EH4"), 0)) {
        handler_data_flags = ExceptionHandlerDataFlag_FuncInfo4;
      } else if (str8_match(handler_name, str8_lit("__CxxFrameHandler4"), 0)) {
        handler_data_flags = ExceptionHandlerDataFlag_FuncInfo4;
      } else if (str8_match(handler_name, str8_lit("__CxxFrameHandler3"), 0)) {
        handler_data_flags = ExceptionHandlerDataFlag_FuncInfo;
      } else if (str8_match(handler_name, str8_lit("__C_specific_handler"), 0)) {
        handler_data_flags = ExceptionHandlerDataFlag_ScopeTable;
      } else if (str8_match(handler_name, str8_lit("__GSHandlerCheck"), 0)) {
        handler_data_flags = ExceptionHandlerDataFlag_GS;
      } else if (str8_match(handler_name, str8_lit("__GSHandlerCheck_SEH"), 0)) {
        handler_data_flags = ExceptionHandlerDataFlag_ScopeTable|ExceptionHandlerDataFlag_GS;
      } else if (str8_match(handler_name, str8_lit("__GSHandlerCheck_EH"), 0)) {
        handler_data_flags = ExceptionHandlerDataFlag_FuncInfo|ExceptionHandlerDataFlag_GS;
      }

      if (handler_data_flags & ExceptionHandlerDataFlag_FuncInfo) {
        MSCRT_FuncInfo func_info;
        read_cursor += mscrt_parse_func_info(arena, raw_data, section_count, sections, read_cursor, &func_info);

        rd_printf("Function Info:");
        rd_indent();
        rd_printf("Magic:                      %#x", func_info.magic);
        rd_printf("Max State:                  %u",  func_info.max_state);
        rd_printf("Try Block Count:            %u",  func_info.try_block_map_count);
        rd_printf("IP Map Count:               %u",  func_info.ip_map_count);
        rd_printf("Frame Offset Unwind Helper: %#x", func_info.frame_offset_unwind_helper);
        rd_printf("ES Flags:                   %#x", func_info.eh_flags);
        rd_unindent();

        if (func_info.ip_map_count > 0) {
          rd_printf("IP to State Map:");
          rd_indent();
          rd_printf("%8s %8s", "State", "IP");
          for (U32 i = 0; i < func_info.ip_map_count; ++i) {
            MSCRT_IPState32 state = func_info.ip_map[i];
            String8 line = str8_zero(); // TODO: syms_line_for_voff(temp.arena, group, state.ip);
            rd_printf("%8d %08x %S", state.state, state.ip, line);
          }
          rd_unindent();
        }

        if (func_info.max_state > 0) {
          rd_printf("Unwind Map:");
          rd_indent();
          rd_printf("%13s  %10s  %8s", "Current State", "Next State", "Action @");
          for (U32 i = 0; i < func_info.max_state; ++i) {
            MSCRT_UnwindMap32 map = func_info.unwind_map[i];
            String8 line = str8_zero(); // TODO: syms_line_for_voff(temp.arena, group, map.action_virt_off);
            rd_printf("%13u  %10d  %8x %S", i, map.next_state, map.action_virt_off, line);
          }
          rd_unindent();
        }

        for (U32 i = 0; i < func_info.try_block_map_count; ++i) {
          MSCRT_TryMapBlock try_block = func_info.try_block_map[i];
          rd_printf("Try Map Block #%u", i);
          rd_indent();
          rd_printf("Try State Low:    %u", try_block.try_low);
          rd_printf("Try State High:   %u", try_block.try_high);
          rd_printf("Catch State High: %u", try_block.catch_high);
          rd_printf("Catch Count:      %u", try_block.catch_handlers_count);
          rd_printf("Catches:");
          rd_indent();
          for (U32 ihandler = 0; ihandler < try_block.catch_handlers_count; ++ihandler) {
            rd_printf("Catch #%u", ihandler);
            rd_indent();
            mscrt_format_eh_handler_type32(arena, out, indent, &try_block.catch_handlers[ihandler]);
            rd_unindent();
          }
          rd_unindent();
          rd_unindent();
        }

        if (func_info.es_type_list.count) {
          rd_printf("Exception Specific Types:");
          rd_indent();
          for (U32 i = 0; i < func_info.es_type_list.count; ++i) {
            if (i > 0) {
              rd_newline();
            }
            mscrt_format_eh_handler_type32(arena, out, indent, &func_info.es_type_list.handlers[i]);
          }
          rd_unindent();
        }
      }
      if (handler_data_flags & ExceptionHandlerDataFlag_FuncInfo4) {
        U32 handler_data_voff = 0;
        read_cursor += str8_deserial_read_struct(raw_data, read_cursor, &handler_data_voff);

        U32 unknown = 0;
        read_cursor += str8_deserial_read_struct(raw_data, read_cursor, &unknown);

        U64                    func_info_foff = coff_foff_from_voff(sections, section_count, handler_data_voff);
        MSCRT_ParsedFuncInfoV4 func_info      = {0};
        mscrt_parse_func_info_v4(arena, raw_data, section_count, sections, func_info_foff, pdata->voff_first, &func_info);

        String8 header_str = str8_zero();
        {
          String8List header_list = {0};
          if (func_info.header & MSCRT_FuncInfoV4Flag_IsCatch) {
            str8_list_pushf(arena, &header_list, "IsCatch");
          }
          if (func_info.header & MSCRT_FuncInfoV4Flag_IsSeparated) {
            str8_list_pushf(arena, &header_list, "IsSeparted");
          }
          if (func_info.header & MSCRT_FuncInfoV4Flag_IsBBT) {
            str8_list_pushf(arena, &header_list, "IsBBT");
          }
          if (func_info.header & MSCRT_FuncInfoV4Flag_UnwindMap) {
            str8_list_pushf(arena, &header_list, "UnwindMap");
          }
          if (func_info.header & MSCRT_FuncInfoV4Flag_TryBlockMap) {
            str8_list_pushf(arena, &header_list, "TryBlockMap");
          }
          if (func_info.header & MSCRT_FuncInfoV4Flag_EHs) {
            str8_list_pushf(arena, &header_list, "EHs");
          }
          if (func_info.header & MSCRT_FuncInfoV4Flag_NoExcept) {
            str8_list_pushf(arena, &header_list, "NoExcept");
          }
          header_str = str8_list_join(arena, &header_list, &(StringJoin){.sep=str8_lit(", ")});
        }

        rd_printf("Function Info V4:");
        rd_indent();
        rd_printf("Header:                %#x %S", func_info.header, header_str);
        rd_printf("BBT Flags:             %#x",    func_info.bbt_flags);

        MSCRT_IP2State32V4 ip2state_map = func_info.ip2state_map;
        rd_printf("IP To State Map:");
        rd_indent();
        rd_printf("%8s %8s", "State", "IP");
        for (U32 i = 0; i < ip2state_map.count; ++i) {
          String8 line_str = str8_zero(); // TODO: syms_line_for_voff(arena, group, ip2state_map.voffs[i]);
          rd_printf("%8d %08X %S", ip2state_map.states[i], ip2state_map.voffs[i], line_str);
        }
        rd_unindent();

        if (func_info.header & MSCRT_FuncInfoV4Flag_UnwindMap) {
          MSCRT_UnwindMapV4 unwind_map = func_info.unwind_map;
          rd_printf("Unwind Map:");
          rd_indent();
          for (U32 i = 0; i < unwind_map.count; ++i) {
            MSCRT_UnwindEntryV4 *ue       = &unwind_map.v[i];
            String8                 type_str = str8_zero();
            switch (ue->type) {
              case MSCRT_UnwindMapV4Type_NoUW:             type_str = str8_lit("NoUW");             break;
              case MSCRT_UnwindMapV4Type_DtorWithObj:      type_str = str8_lit("DtorWithObj");      break;
              case MSCRT_UnwindMapV4Type_DtorWithPtrToObj: type_str = str8_lit("DtorWithPtrToObj"); break;
              case MSCRT_UnwindMapV4Type_VOFF:             type_str = str8_lit("VOFF");             break;
            }
            if (ue->type == MSCRT_UnwindMapV4Type_DtorWithObj || ue->type == MSCRT_UnwindMapV4Type_DtorWithPtrToObj) {
              rd_printf("[%2u] NextOff=%u Type=%-16S Action=%#08x Object=%#x", i, ue->next_off, type_str, ue->action, ue->object);
            } else if (ue->type == MSCRT_UnwindMapV4Type_VOFF) {
              rd_printf("[%2u] NextOff=%u Type=%-16S Action=%#08x", i, ue->next_off, type_str, ue->action);
            } else {
              rd_printf("[%2u] NextOff=%u Type=%S", i, ue->next_off, type_str);
            }
          }
          rd_unindent();
        }

        if (func_info.header & MSCRT_FuncInfoV4Flag_TryBlockMap) {
          MSCRT_TryBlockMapV4Array try_block_map = func_info.try_block_map;
          rd_printf("Try/Catch Blocks:");
          rd_indent();
          for (U32 i = 0; i < try_block_map.count; ++i) {
            MSCRT_TryBlockMapV4 *try_block = &try_block_map.v[i];
            rd_printf("[%2u] TryLow %u TryHigh %u CatchHigh %u", i, try_block->try_low, try_block->try_high, try_block->catch_high);
            if (try_block->handlers.count) {
              for (U32 k = 0; k < try_block->handlers.count; ++k) {
                MSCRT_EhHandlerTypeV4 *handler = &try_block->handlers.v[k];

                String8List line_list = {0};
                str8_list_pushf(arena, &line_list, "  ");
                str8_list_pushf(arena, &line_list, "CatchCodeVOff=%#08X", handler->catch_code_voff);
                if (handler->flags & MSCRT_EhHandlerV4Flag_Adjectives) {
                  String8 adjectives = mscrt_string_from_eh_adjectives(arena, handler->adjectives);
                  str8_list_pushf(arena, &line_list, "Adjectives=%S", adjectives);
                }
                if (handler->flags & MSCRT_EhHandlerV4Flag_DispType) {
                  str8_list_pushf(arena, &line_list, "TypeVOff=%#x", handler->type_voff);
                }
                if (handler->flags & MSCRT_EhHandlerV4Flag_DispCatchObj) {
                  str8_list_pushf(arena, &line_list, "CacthObjVOff=%#x", handler->catch_obj_voff);
                }
                if (handler->flags & MSCRT_EhHandlerV4Flag_ContIsVOff) {
                  str8_list_pushf(arena, &line_list, "ContIsVOff");
                }
                for (U32 icont = 0; icont < handler->catch_funclet_cont_addr_count; ++icont) {
                  str8_list_pushf(arena, &line_list, "ContAddr[%u]=%#llx", icont, handler->catch_funclet_cont_addr[icont]);
                }

                String8 handler_str = str8_list_join(arena, &line_list, &(StringJoin){.sep=str8_lit(" ")});
                rd_printf("%S", handler_str);
              }
            }
          }
          rd_unindent();
        }
      }
      if (handler_data_flags & ExceptionHandlerDataFlag_ScopeTable) {
        U32 scope_count = 0;
        read_cursor += str8_deserial_read_struct(raw_data, read_cursor, &scope_count);

        PE_HandlerScope *scopes = str8_deserial_get_raw_ptr(raw_data, read_cursor, sizeof(PE_HandlerScope)*scope_count); 
        read_cursor += scope_count*sizeof(scopes[0]);

        rd_printf("Count of scope table entries: %u", scope_count);
        rd_indent();
        rd_printf("%-8s %-8s %-8s %-8s", "Begin", "End", "Handler", "Target");
        for (U32 i = 0; i < scope_count; ++i) {
          PE_HandlerScope scope = scopes[i];
          rd_printf("%08x %08x %08x %08x", scope.begin, scope.end, scope.handler, scope.target);
        }
        rd_unindent();
      }
      if (handler_data_flags & ExceptionHandlerDataFlag_GS) {
        U32 gs_data = 0;
        read_cursor += str8_deserial_read_struct(raw_data, read_cursor, &gs_data);

        U32 flags               = MSCRT_GSHandler_GetFlags(gs_data);
        U32 cookie_offset       = MSCRT_GSHandler_GetCookieOffset(gs_data);
        U32 aligned_base_offset = 0;
        U32 alignment           = 0;
        if (flags & MSCRT_GSHandlerFlag_HasAlignment) {
          read_cursor += str8_deserial_read_struct(raw_data, read_cursor, &aligned_base_offset);
          read_cursor += str8_deserial_read_struct(raw_data, read_cursor, &alignment);
        }

        String8 flags_str;
        {
          String8List flags_list = {0};
          if (flags & MSCRT_GSHandlerFlag_EHandler) {
            str8_list_pushf(arena, &flags_list, "EHandler");
          }
          if (flags & MSCRT_GSHandlerFlag_UHandler) {
            str8_list_pushf(arena, &flags_list, "UHandler");
          }
          if (flags & MSCRT_GSHandlerFlag_HasAlignment) {
            str8_list_pushf(arena, &flags_list, "Has Alignment");
          }
          if (flags == 0) {
            str8_list_pushf(arena, &flags_list, "None");
          }
          flags_str = str8_list_join(arena, &flags_list, &(StringJoin){.sep=str8_lit(", ")});
        }
        rd_printf("GS unwind flags:     %S", flags_str);
        rd_printf("Cookie offset:       %x", cookie_offset);
        if (flags & MSCRT_GSHandlerFlag_HasAlignment) {
          rd_printf("Aligned base offset: %x", aligned_base_offset);
          rd_printf("Alignment:           %x", alignment);
        }
      }

      #undef ExceptionHandlerDataFlag_FuncInfo
      #undef ExceptionHandlerDataFlag_ScopeTable
      #undef ExceptionHandlerDataFlag_GS
    }

    temp_end(temp);
  } 

  scratch_end(scratch);
}

internal void
pe_format_exceptions(Arena              *arena,
                     String8List        *out,
                     String8             indent,
                     COFF_MachineType    machine,
                     U64                 section_count,
                     COFF_SectionHeader *sections,
                     String8             raw_data,
                     Rng1U64             except_frange)
{
  if (dim_1u64(except_frange)) {
    rd_printf("# Exceptions");
    rd_indent();
    rd_printf("%-8s %-8s %-8s %-8s", "Offset", "Begin", "End", "Unwind Info");

    switch (machine) {
      case COFF_MachineType_UNKNOWN: break;
      case COFF_MachineType_X64:
      case COFF_MachineType_X86: {
        pe_format_exceptions_x8664(arena, out, indent, section_count, sections, raw_data, except_frange);
      } break;
      default: NotImplemented; break;
    }
    rd_unindent();
    rd_newline();
  }
}

internal void
pe_format_base_relocs(Arena              *arena,
                      String8List        *out,
                      String8             indent,
                      COFF_MachineType    machine,
                      U64                 image_base,
                      U64                 section_count,
                      COFF_SectionHeader *sections,
                      String8             raw_data,
                      Rng1U64             base_reloc_franges)
{
  Temp scratch = scratch_begin(&arena, 1);

  String8               raw_base_relocs = str8_substr(raw_data, base_reloc_franges);
  PE_BaseRelocBlockList base_relocs     = pe_base_reloc_block_list_from_data(scratch.arena, raw_base_relocs);

  if (base_relocs.count) {
    rd_printf("# Base Relocs");
    rd_indent();

    U32 addr_size = 0;
    switch (machine) {
      case COFF_MachineType_UNKNOWN: break;
      case COFF_MachineType_X86:     addr_size = 4; break;
      case COFF_MachineType_X64:     addr_size = 8; break;
      default: NotImplemented;
    }
   
    // convert blocks to string list
    U64 iblock = 0;
    for (PE_BaseRelocBlockNode *node = base_relocs.first; node != 0; node = node->next) {
      PE_BaseRelocBlock *block = &node->v;
      rd_printf("Block No. %u, Virt Off %#x, Reloc Count %u", iblock++, block->page_virt_off, block->entry_count);
      rd_indent();
      for (U64 ientry = 0; ientry < block->entry_count; ++ientry) {
        PE_BaseRelocKind type   = PE_BaseRelocKindFromEntry(block->entries[ientry]);
        U16              offset = PE_BaseRelocOffsetFromEntry(block->entries[ientry]);
        
        U64 apply_to_voff = block->page_virt_off + offset;
        U64 apply_to_foff = coff_foff_from_voff(sections, section_count, apply_to_voff);
        U64 apply_to      = 0;
        str8_deserial_read(raw_data, apply_to_foff, &apply_to, addr_size, 1);
        U64 addr = image_base + apply_to;
        
        const char *type_str = "???";
        switch (type) {
          case PE_BaseRelocKind_ABSOLUTE: type_str = "ABS";     break;
          case PE_BaseRelocKind_HIGH:     type_str = "HIGH";    break;
          case PE_BaseRelocKind_LOW:      type_str = "LOW";     break;
          case PE_BaseRelocKind_HIGHLOW:  type_str = "HIGHLOW"; break;
          case PE_BaseRelocKind_HIGHADJ:  type_str = "HIGHADJ"; break;
          case PE_BaseRelocKind_DIR64:    type_str = "DIR64";   break;
          default: {
            switch (machine) {
              case COFF_MachineType_ARM:
              case COFF_MachineType_ARM64:
              case COFF_MachineType_ARMNT: {
                switch (type) {
                  case PE_BaseRelocKind_ARM_MOV32:   type_str = "ARM_MOV32";   break;
                  case PE_BaseRelocKind_THUMB_MOV32: type_str = "THUMB_MOV32"; break;
                  default: NotImplemented;
                }
              } break;
              // TODO: mips, loong, risc-v
            }
          } break;
        }
        
        if (type == PE_BaseRelocKind_ABSOLUTE) {
          rd_printf("%-4x %-12s", offset, type_str);
        } else {
          rd_printf("%-4x %-12s %016llx", offset, type_str, apply_to);

          // TODO
          #if 0
          U64 reloc_voff = apply_to - image_base;
          SYMS_UnitID   uid = syms_group_uid_from_voff__accelerated(group, reloc_voff);
          SYMS_SymbolID sid = syms_group_proc_sid_from_uid_voff__accelerated(group, uid, reloc_voff);
          if (sid) {
            SYMS_UnitAccel *unit = syms_group_unit_from_uid(group, uid);
            String8 name = syms_group_symbol_name_from_sid(arena, group, unit, sid);
            str8_list_pushf(arena, list, "  %-4X %-12s %016llX %.*s", offset, type_str, apply_to, syms_expand_string(name));
          } else {
            str8_list_pushf(arena, list, "  %-4X %-12s %016llX", offset, type_str, apply_to);
          }
          #endif
        }
      }
      rd_unindent();
      rd_newline();
    }

    rd_unindent();
  }

  scratch_end(scratch);
}

internal void
pe_format(Arena *arena, String8List *out, String8 indent, String8 raw_data, RD_Option opts)
{
  Temp scratch = scratch_begin(&arena, 1);

  PE_DosHeader *dos_header = str8_deserial_get_raw_ptr(raw_data, 0, sizeof(*dos_header));
  if (!dos_header) {
    rd_errorf("not enough bytes to read DOS header");
    goto exit;
  }
  Assert(dos_header->magic == PE_DOS_MAGIC);

  U32 pe_magic = 0;
  str8_deserial_read_struct(raw_data, dos_header->coff_file_offset, &pe_magic);
  if (pe_magic != PE_MAGIC) {
    rd_errorf("PE magic check failure, input file is not of PE format");
    goto exit;
  }

  U64          coff_header_off = dos_header->coff_file_offset+sizeof(pe_magic);
  COFF_Header *coff_header     = str8_deserial_get_raw_ptr(raw_data, coff_header_off, sizeof(*coff_header));
  if (!coff_header) {
    rd_errorf("not enough bytes to read COFF header");
    goto exit;
  }

  U64 opt_header_off   = coff_header_off + sizeof(*coff_header);
  U16 opt_header_magic = 0;
  str8_deserial_read_struct(raw_data, opt_header_off, &opt_header_magic);
  if (opt_header_magic != PE_PE32_MAGIC && opt_header_magic != PE_PE32PLUS_MAGIC) {
    rd_errorf("unexpected optional header magic %#x", opt_header_magic);
    goto exit;
  }

  if (opt_header_magic == PE_PE32_MAGIC && coff_header->optional_header_size < sizeof(PE_OptionalHeader32)) {
    rd_errorf("unexpected optional header size in COFF header %m, expected at least %m", coff_header->optional_header_size, sizeof(PE_OptionalHeader32));
    goto exit;
  }

  if (opt_header_magic == PE_PE32PLUS_MAGIC && coff_header->optional_header_size < sizeof(PE_OptionalHeader32Plus)) {
    rd_errorf("unexpected optional header size %m, expected at least %m", coff_header->optional_header_size, sizeof(PE_OptionalHeader32Plus));
    goto exit;
  }

  U64                 sections_off = coff_header_off + sizeof(*coff_header) + coff_header->optional_header_size;
  COFF_SectionHeader *sections     = str8_deserial_get_raw_ptr(raw_data, sections_off, sizeof(*sections)*coff_header->section_count);
  if (!sections) {
    rd_errorf("not enough bytes to read COFF section headers");
    goto exit;
  }

  U64 string_table_off = coff_header->symbol_table_foff + sizeof(COFF_Symbol16) * coff_header->symbol_count;

  COFF_Symbol32Array symbols = coff_symbol_array_from_data_16(scratch.arena, raw_data, coff_header->symbol_table_foff, coff_header->symbol_count);

  U8 *raw_opt_header = push_array(scratch.arena, U8, coff_header->optional_header_size);
  str8_deserial_read_array(raw_data, opt_header_off, raw_opt_header, coff_header->optional_header_size);

  if (opts & RD_Option_Headers) {
    coff_format_header(arena, out, indent, coff_header);
    rd_newline();
  }
  
  U64               image_base = 0;
  U64               dir_count  = 0;
  PE_DataDirectory *dirs       = 0;

  if (opt_header_magic == PE_PE32_MAGIC) {
    PE_OptionalHeader32 *opt_header = (PE_OptionalHeader32 *)raw_opt_header;
    image_base = opt_header->image_base;
    dir_count  = opt_header->data_dir_count;
    dirs       = str8_deserial_get_raw_ptr(raw_data, opt_header_off+sizeof(*opt_header), sizeof(*dirs) * opt_header->data_dir_count);
    if (!dirs) {
      rd_errorf("unable to read data directories");
      goto exit;
    }

    if (opts & RD_Option_Headers) {
      pe_format_optional_header32(arena, out, indent, opt_header, dirs);
    }
  } else if (opt_header_magic == PE_PE32PLUS_MAGIC) {
    PE_OptionalHeader32Plus *opt_header = (PE_OptionalHeader32Plus *)raw_opt_header;
    image_base = opt_header->image_base;
    dir_count  = opt_header->data_dir_count;
    dirs       = str8_deserial_get_raw_ptr(raw_data, opt_header_off+sizeof(*opt_header), sizeof(*dirs) * opt_header->data_dir_count);
    if (!dirs) {
      rd_errorf("unable to read data directories");
      goto exit;
    }

    if (opts & RD_Option_Headers) {
      pe_format_optional_header32plus(arena, out, indent, opt_header, dirs);
    }
  }

  // map data directory RVA to file offsets
  Rng1U64 *dirs_file_ranges = push_array(scratch.arena, Rng1U64, dir_count);
  Rng1U64 *dirs_virt_ranges = push_array(scratch.arena, Rng1U64, dir_count);
  for (U64 i = 0; i < dir_count; ++i) {
    PE_DataDirectory dir = dirs[i];
    U64 file_off = coff_foff_from_voff(sections, coff_header->section_count, dir.virt_off);
    dirs_file_ranges[i] = r1u64(file_off, file_off+dir.virt_size);
    dirs_virt_ranges[i] = r1u64(dir.virt_off, dir.virt_off+dir.virt_size);
  }

  if (opts & RD_Option_Sections) {
    coff_format_section_table(arena, out, indent, raw_data, string_table_off, symbols, coff_header->section_count, sections);
  }

  if (opts & RD_Option_Relocs) {
    coff_format_relocs(arena, out, indent, raw_data, string_table_off, coff_header->machine, coff_header->section_count, sections, symbols);
  }

  if (opts & RD_Option_Symbols) {
    coff_format_symbol_table(arena, out, indent, raw_data, 0, string_table_off, symbols);
  }

  if (opts & RD_Option_Exports) {
    PE_ParsedExportTable exptab = pe_exports_from_data(arena,
                                                       coff_header->section_count,
                                                       sections,
                                                       raw_data,
                                                       dirs_file_ranges[PE_DataDirectoryIndex_EXPORT],
                                                       dirs_virt_ranges[PE_DataDirectoryIndex_EXPORT]);
  }

  if (opts & RD_Option_Imports) {
    B32                        is_pe32       = opt_header_magic == PE_PE32_MAGIC;
    PE_ParsedStaticImportTable static_imptab = pe_static_imports_from_data(arena, is_pe32, coff_header->section_count, sections, raw_data, dirs_file_ranges[PE_DataDirectoryIndex_IMPORT]);
    PE_ParsedDelayImportTable  delay_imptab  = pe_delay_imports_from_data(arena, is_pe32, coff_header->section_count, sections, raw_data, dirs_file_ranges[PE_DataDirectoryIndex_DELAY_IMPORT]);
    pe_format_static_import_table(arena, out, indent, image_base, static_imptab);
    pe_format_delay_import_table(arena, out, indent, image_base, delay_imptab);
  }

  if (opts & RD_Option_Resources) {
    String8         raw_dir  = str8_substr(raw_data, dirs_file_ranges[PE_DataDirectoryIndex_RESOURCES]);
    PE_ResourceDir *dir_root = pe_resource_table_from_directory_data(scratch.arena, raw_dir);
    pe_format_resources(arena, out, indent, dir_root);
  }

  if (opts & RD_Option_Exceptions) {
    pe_format_exceptions(arena, out, indent, coff_header->machine, coff_header->section_count, sections, raw_data, dirs_file_ranges[PE_DataDirectoryIndex_EXCEPTIONS]);
  }

  if (opts & RD_Option_Relocs) {
    pe_format_base_relocs(arena, out, indent, coff_header->machine, image_base, coff_header->section_count, sections, raw_data, dirs_file_ranges[PE_DataDirectoryIndex_BASE_RELOC]);
  }

  if (opts & RD_Option_Debug) {
    if (PE_DataDirectoryIndex_DEBUG < dir_count) {
      String8 raw_dir = str8_substr(raw_data, dirs_file_ranges[PE_DataDirectoryIndex_DEBUG]);
      pe_format_debug_directory(arena, out, indent, raw_data, raw_dir);
    }
  }

  if (opts & RD_Option_Tls) {
    if (dim_1u64(dirs_file_ranges[PE_DataDirectoryIndex_TLS])) {
      PE_ParsedTLS tls = pe_tls_from_data(scratch.arena, coff_header->machine, image_base, coff_header->section_count, sections, raw_data, dirs_file_ranges[PE_DataDirectoryIndex_TLS]);
      pe_format_tls(arena, out, indent, tls);
    }
  }

  if (opts & RD_Option_LoadConfig) {
    String8 raw_lc = str8_substr(raw_data, dirs_file_ranges[PE_DataDirectoryIndex_LOAD_CONFIG]);
    if (raw_lc.size) {
      switch (coff_header->machine) {
        case COFF_MachineType_UNKNOWN: break;
        case COFF_MachineType_X86: {
          PE_LoadConfig32 *lc = str8_deserial_get_raw_ptr(raw_lc, 0, sizeof(*lc));
          if (lc) {
            pe_format_load_config32(arena, out, indent, lc);
          } else {
            rd_errorf("not enough bytes to parse 32bit load config");
          }
        } break;
        case COFF_MachineType_X64: {
          PE_LoadConfig64 *lc = str8_deserial_get_raw_ptr(raw_lc, 0, sizeof(*lc));
          if (lc) {
            pe_format_load_config64(arena, out, indent, lc);
          } else {
            rd_errorf("not enough bytes to parse 64bit load config");
          }
        } break;
        default: NotImplemented;
      }
    }
  }

  RD_MarkerArray *section_markers = 0;
  if (opts & (RD_Option_Disasm|RD_Option_Rawdata)) {
    section_markers = rd_section_markers_from_coff_symbol_table(scratch.arena, raw_data, string_table_off, coff_header->section_count, symbols);
  }

  if (opts & RD_Option_Rawdata) {
    coff_raw_data_sections(arena, out, indent, raw_data, 0, section_markers, coff_header->section_count, sections);
  }

  if (opts & RD_Option_Disasm) {
    coff_disasm_sections(arena, out, indent, raw_data, coff_header->machine, 0, 1, section_markers, coff_header->section_count, sections);
  }

exit:;
  scratch_end(scratch);
}

internal B32
is_pe(String8 raw_data)
{
  PE_DosHeader header = {0};
  str8_deserial_read_struct(raw_data, 0, &header);
  return header.magic == PE_DOS_MAGIC;
}

internal void
format_preamble(Arena *arena, String8List *out, String8 indent, String8 input_path, String8 raw_data)
{
  Temp scratch = scratch_begin(&arena, 1);

  char *input_type_string = "???";

  if (coff_is_archive(raw_data)) {
    input_type_string = "Archive";
  } else if (coff_is_thin_archive(raw_data)) {
    input_type_string = "Thin Archive";
  } else if (coff_is_big_obj(raw_data)) {
    input_type_string = "Big Obj";
  } else if (coff_is_obj(raw_data)) {
    input_type_string = "Obj";
  } else if (is_pe(raw_data)) {
    input_type_string = "COFF/PE";
  }

  DateTime universal_dt = os_now_universal_time();
  DateTime local_dt     = os_local_time_from_universal(&universal_dt);
  String8  time = push_date_time_string(scratch.arena, &local_dt);

  rd_printf("# Input");
  rd_indent();
  rd_printf("Path: %S", input_path);
  rd_printf("Type: %s", input_type_string);
  rd_printf("Time: %S", time);
  rd_unindent();
  rd_newline();

  scratch_end(scratch);
}
