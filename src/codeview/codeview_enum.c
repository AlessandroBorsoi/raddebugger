// Copyright (c) 2024 Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

internal String8
cv_string_from_type_index_source(CV_TypeIndexSource ti_source)
{
  switch (ti_source) {
  case CV_TypeIndexSource_NULL:  return str8_lit("");    break;
  case CV_TypeIndexSource_TPI:   return str8_lit("TPI"); break;
  case CV_TypeIndexSource_IPI:   return str8_lit("IPI"); break;
  case CV_TypeIndexSource_COUNT: break;
  }
  return str8_zero();
}

internal String8
cv_string_from_language(CV_Language x)
{
  switch (x) {
    #define X(_n,_i) case _i: return str8_lit(Stringify(_n));
CV_LanguageXList(X)
    #undef X
  }
  return str8_zero();
}

internal String8
cv_string_from_numeric(Arena *arena, CV_NumericParsed num)
{
  String8 result = str8_zero();
  switch (num.kind) {
    case CV_NumericKind_FLOAT16:   break; // TODO: format float16
    case CV_NumericKind_FLOAT32:   result = push_str8f(arena, "%f", (F64)(*(F32*)num.val)); break;
    case CV_NumericKind_FLOAT48:   break; // TODO: format float48
    case CV_NumericKind_FLOAT64:   result = push_str8f(arena, "%f", *(F64*)num.val); break;
    case CV_NumericKind_FLOAT80:   break; // TODO: format float80
    case CV_NumericKind_FLOAT128:  break; // TODO: format float128
    case CV_NumericKind_CHAR:      result = push_str8f(arena, "%d",   *(S8 *)num.val); break;
    case CV_NumericKind_SHORT:     result = push_str8f(arena, "%d",   *(S16*)num.val); break;
    case CV_NumericKind_LONG:      result = push_str8f(arena, "%d",   *(S32*)num.val); break;
    case CV_NumericKind_QUADWORD:  result = push_str8f(arena, "%lld", *(S64*)num.val); break;
    case CV_NumericKind_USHORT:    result = push_str8f(arena, "%u",   *(U16*)num.val); break;
    case CV_NumericKind_ULONG:     result = push_str8f(arena, "%u",   *(U32*)num.val); break;
    case CV_NumericKind_UQUADWORD: result = push_str8f(arena, "%llu", *(U64*)num.val); break;
  }
  return result;
}

internal String8 
cv_string_from_reg_id(CV_Arch arch, U32 id)
{
  String8 result = str8_zero();
  switch (arch) {
  case CV_Arch_8086: {
      switch (id) {
      #define X(_name, _id,...) case _id: str8_lit(Stringify(_name)); break;
      CV_Reg_X86_XList(X)
      #undef X
      }
  } break;
  case CV_Arch_X64: {
      switch (id) {
      #define X(_name, _id,...) case _id: str8_lit(Stringify(_name)); break;
      CV_Reg_X64_XList(X)
      #undef X
      }
  } break;
  default: NotImplemented;
  }
  return result;
}

internal String8
cv_string_from_member_access(CV_MemberAccess x)
{
  switch (x) {
    case CV_MemberAccess_Null:      break;
    case CV_MemberAccess_Private:   return str8_lit("Private");
    case CV_MemberAccess_Protected: return str8_lit("Protected");
    case CV_MemberAccess_Public:    return str8_lit("Public");
  }
  return str8_zero();
}

internal String8
cv_string_from_method_prop(CV_MethodProp x)
{
  switch (x) {
    case CV_MethodProp_Vanilla:     return str8_lit("Vanilla");
    case CV_MethodProp_Virtual:     return str8_lit("Virtual");
    case CV_MethodProp_Static:      return str8_lit("Static");
    case CV_MethodProp_Friend:      return str8_lit("Friend");
    case CV_MethodProp_Intro:       return str8_lit("Intro");
    case CV_MethodProp_PureVirtual: return str8_lit("PureVirtual");
    case CV_MethodProp_PureIntro:   return str8_lit("PureIntro");
  }
  return str8_zero();
}

internal String8
cv_string_from_hfa(CV_HFAKind x)
{
  switch (x) {
    case CV_HFAKind_None:   return str8_lit("None");
    case CV_HFAKind_Float:  return str8_lit("Float");
    case CV_HFAKind_Double: return str8_lit("Double");
    case CV_HFAKind_Other:  return str8_lit("Other");
  }
  return str8_zero();
}

internal String8
cv_string_from_mcom(CV_MoComUDTKind x)
{
  switch (x) {
    case CV_MoComUDTKind_None:      return str8_lit("None");
    case CV_MoComUDTKind_Ref:       return str8_lit("Ref");
    case CV_MoComUDTKind_Value:     return str8_lit("Value");
    case CV_MoComUDTKind_Interface: return str8_lit("Interface");
  }
  return str8_zero();
}

internal String8
cv_string_from_binary_opcode(CV_InlineBinaryAnnotation x)
{
  switch (x) {
    case CV_InlineBinaryAnnotation_Null:                          break;
    case CV_InlineBinaryAnnotation_CodeOffset:                    return str8_lit("CodeOffset");
    case CV_InlineBinaryAnnotation_ChangeCodeOffsetBase:          return str8_lit("ChangeCodeOffsetBase");
    case CV_InlineBinaryAnnotation_ChangeCodeOffset:              return str8_lit("ChangeCodeOffset");
    case CV_InlineBinaryAnnotation_ChangeCodeLength:              return str8_lit("ChangeCodeLength");
    case CV_InlineBinaryAnnotation_ChangeFile:                    return str8_lit("ChangeFile");
    case CV_InlineBinaryAnnotation_ChangeLineOffset:              return str8_lit("ChangeLineOffset");
    case CV_InlineBinaryAnnotation_ChangeLineEndDelta:            return str8_lit("ChangeLineEndDelta");
    case CV_InlineBinaryAnnotation_ChangeRangeKind:               return str8_lit("ChangeRangeKind");
    case CV_InlineBinaryAnnotation_ChangeColumnStart:             return str8_lit("ChangeColumnStart");
    case CV_InlineBinaryAnnotation_ChangeColumnEndDelta:          return str8_lit("ChangeColumnEndDelta");
    case CV_InlineBinaryAnnotation_ChangeCodeOffsetAndLineOffset: return str8_lit("ChangeCodeOffsetAndLineOffset");
    case CV_InlineBinaryAnnotation_ChangeCodeLengthAndCodeOffset: return str8_lit("ChangeCodeLengthAndCodeOffset");
    case CV_InlineBinaryAnnotation_ChangeColumnEnd:               return str8_lit("ChangeColumnEnd");
  }
  return str8_zero();
}

internal String8
cv_string_from_thunk_ordinal(CV_ThunkOrdinal x)
{
  switch (x) {
    case CV_ThunkOrdinal_NoType:            return str8_lit("NoType");
    case CV_ThunkOrdinal_Adjustor:          return str8_lit("Adjustor");
    case CV_ThunkOrdinal_VCall:             return str8_lit("VCall");
    case CV_ThunkOrdinal_PCode:             return str8_lit("PCode");
    case CV_ThunkOrdinal_Load:              return str8_lit("Load");
    case CV_ThunkOrdinal_TrampIncremental:  return str8_lit("TrampIncremental");
    case CV_ThunkOrdinal_TrampBranchIsland: return str8_lit("TrampBranchIsland");
  }
  return str8_zero();
}

internal String8
cv_string_from_frame_cookie_kind(CV_FrameCookieKind x)
{
  switch (x) {
    case CV_FrameCookieKind_Copy:  return str8_lit("Copy");
    case CV_FrameCookieKind_XorSP: return str8_lit("XorSP");
    case CV_FrameCookieKind_XorBP: return str8_lit("XorR13");
  }
  return str8_zero();
}

internal String8
cv_string_from_generic_style(CV_GenericStyle x)
{
  switch (x) {
    case CV_GenericStyle_VOID:   return str8_lit("VOID");
    case CV_GenericStyle_REG:    return str8_lit("REG");
    case CV_GenericStyle_ICAN:   return str8_lit("ICAN");
    case CV_GenericStyle_ICAF:   return str8_lit("ICAF");
    case CV_GenericStyle_IRAN:   return str8_lit("IRAN");
    case CV_GenericStyle_IRAF:   return str8_lit("IRAF");
    case CV_GenericStyle_UNUSED: return str8_lit("UNUSED");
  }
  return str8_zero();
}

internal String8
cv_string_from_trampoline_kind(CV_TrampolineKind x)
{
  switch (x) {
    case CV_TrampolineKind_Incremental:  return str8_lit("Incremental");
    case CV_TrampolineKind_BranchIsland: return str8_lit("BranchIsland");
  }
  return str8_zero();
}

internal String8
cv_string_from_virtual_table_shape_kind(CV_VirtualTableShape x)
{
  switch (x) {
    case CV_VirtualTableShape_Near:   return str8_lit("Near");
    case CV_VirtualTableShape_Far:    return str8_lit("Far");
    case CV_VirtualTableShape_Thin:   return str8_lit("Thin");
    case CV_VirtualTableShape_Outer:  return str8_lit("Outer");
    case CV_VirtualTableShape_Meta:   return str8_lit("Meta");
    case CV_VirtualTableShape_Near32: return str8_lit("Near32");
    case CV_VirtualTableShape_Far32:  return str8_lit("Far32");
  }
  return str8_zero();
}

internal String8
cv_string_from_call_kind(CV_CallKind x)
{
  switch (x) {
    case CV_CallKind_NearC:          return str8_lit("NearC");
    case CV_CallKind_FarC:           return str8_lit("FarC");
    case CV_CallKind_NearPascal:     return str8_lit("NearPascal");
    case CV_CallKind_FarPascal:      return str8_lit("FarPascal");
    case CV_CallKind_NearFast:       return str8_lit("NearFast");
    case CV_CallKind_FarFast:        return str8_lit("FarFast");
    case CV_CallKind_UNUSED:         return str8_lit("UNUSED");
    case CV_CallKind_NearStd:        return str8_lit("NearStd");
    case CV_CallKind_FarStd:         return str8_lit("FarStd");
    case CV_CallKind_NearSys:        return str8_lit("NearSys");
    case CV_CallKind_FarSys:         return str8_lit("FarSys");
    case CV_CallKind_This:           return str8_lit("This");
    case CV_CallKind_Mips:           return str8_lit("Mips");
    case CV_CallKind_Generic:        return str8_lit("Generic");
    case CV_CallKind_Alpha:          return str8_lit("Alpha");
    case CV_CallKind_PPC:            return str8_lit("PPC");
    case CV_CallKind_HitachiSuperH:  return str8_lit("HitachiSuperH");
    case CV_CallKind_Arm:            return str8_lit("Arm");
    case CV_CallKind_AM33:           return str8_lit("AM33");
    case CV_CallKind_TriCore:        return str8_lit("TriCore");
    case CV_CallKind_HitachiSuperH5: return str8_lit("HitachiSuperH5");
    case CV_CallKind_M32R:           return str8_lit("M32R");
    case CV_CallKind_Clr:            return str8_lit("Clr");
    case CV_CallKind_Inline:         return str8_lit("Inline");
    case CV_CallKind_NearVector:     return str8_lit("NearVector");
  }
  return str8_zero();
}

internal String8
cv_string_from_member_pointer_kind(CV_MemberPointerKind x)
{
  switch (x) {
    case CV_MemberPointerKind_Undef:        return str8_lit("Undef");
    case CV_MemberPointerKind_DataSingle:   return str8_lit("DataSingle");
    case CV_MemberPointerKind_DataMultiple: return str8_lit("DataMultiple");
    case CV_MemberPointerKind_DataVirtual:  return str8_lit("DataVirtual");
    case CV_MemberPointerKind_DataGeneral:  return str8_lit("DataGeneral");
    case CV_MemberPointerKind_FuncSingle:   return str8_lit("FuncSingle");
    case CV_MemberPointerKind_FuncMultiple: return str8_lit("FuncMultiple");
    case CV_MemberPointerKind_FuncGeneral:  return str8_lit("FuncGeneral");
  }
  return str8_zero();
}

internal String8
cv_string_from_pointer_kind(CV_PointerKind x)
{
  switch (x) {
    case CV_PointerKind_Near:        return str8_lit("Near");
    case CV_PointerKind_Far:         return str8_lit("Far");
    case CV_PointerKind_Huge:        return str8_lit("Huge");
    case CV_PointerKind_BaseSeg:     return str8_lit("BaseSeg");
    case CV_PointerKind_BaseVal:     return str8_lit("BaseVal");
    case CV_PointerKind_BaseSegVal:  return str8_lit("BaseSegVal");
    case CV_PointerKind_BaseAddr:    return str8_lit("BaseAddr");
    case CV_PointerKind_BaseSegAddr: return str8_lit("BaseSegAddr");
    case CV_PointerKind_BaseType:    return str8_lit("BaseType");
    case CV_PointerKind_BaseSelf:    return str8_lit("BaseSelf");
    case CV_PointerKind_Near32:      return str8_lit("Near32");
    case CV_PointerKind_Far32:       return str8_lit("Far32");
    case CV_PointerKind_64:          return str8_lit("64");
  }
  return str8_zero();
}

internal String8
cv_string_from_pointer_mode(CV_PointerMode x)
{
  switch (x) {
    case CV_PointerMode_Ptr:       return str8_lit("Ptr");
    case CV_PointerMode_LRef:      return str8_lit("LRef");
    case CV_PointerMode_PtrMem:    return str8_lit("PtrMem");
    case CV_PointerMode_PtrMethod: return str8_lit("PtrMethod");
    case CV_PointerMode_RRef:      return str8_lit("RRef");
  }
  return str8_zero();
}

internal String8
cv_string_from_c13_checksum_kind(CV_C13ChecksumKind x)
{
  switch (x) {
    case CV_C13ChecksumKind_Null:   break;
    case CV_C13ChecksumKind_MD5:    return str8_lit("MD5");
    case CV_C13ChecksumKind_SHA1:   return str8_lit("SHA1");
    case CV_C13ChecksumKind_SHA256: return str8_lit("SHA256");
  }
  return str8_zero();
}

internal String8
cv_string_from_c13_subsection_kind(CV_C13SubSectionKind x)
{
  switch (x) {
    #define X(_id, _name) case CV_C13SubSectionKind_##_id: return str8_lit(Stringify(_name));
    CV_C13SubSectionKindXList(X)
    #undef X
  }
  return str8_zero();
}

internal String8
cv_string_from_modifier_flags(Arena *arena, CV_ModifierFlags x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_ModifierFlag_Const) {
    str8_list_pushf(scratch.arena, &list, "Const");
  }
  if (x & CV_ModifierFlag_Volatile) {
    str8_list_pushf(scratch.arena, &list, "Volatile");
  }
  if (x & CV_ModifierFlag_Unaligned) {
    str8_list_pushf(scratch.arena, &list, "Unaligned");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
}

internal String8
cv_string_from_pointer_attribs(Arena *arena, CV_PointerAttribs x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_PointerAttrib_IsFlat) {
    str8_list_pushf(scratch.arena, &list, "IsFlat");
  }
  if (x & CV_PointerAttrib_Volatile) {
    str8_list_pushf(scratch.arena, &list, "Volatile");
  }
  if (x & CV_PointerAttrib_Const) {
    str8_list_pushf(scratch.arena, &list, "Const");
  }
  if (x & CV_PointerAttrib_Unaligned) {
    str8_list_pushf(scratch.arena, &list, "Unaligned");
  }
  if (x & CV_PointerAttrib_Restricted) {
    str8_list_pushf(scratch.arena, &list, "Restricted");
  }
  if (x & CV_PointerAttrib_MOCOM) {
    str8_list_pushf(scratch.arena, &list, "MOCOM");
  }
  if (x & CV_PointerAttrib_LRef) {
    str8_list_pushf(scratch.arena, &list, "LRef");
  }
  if (x & CV_PointerAttrib_RRef) {
    str8_list_pushf(scratch.arena, &list, "RRef");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_function_attribs(Arena *arena, CV_FunctionAttribs x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_FunctionAttrib_CxxReturnUDT) {
    str8_list_pushf(scratch.arena, &list, "CxxReturnUDT");
  }
  if (x & CV_FunctionAttrib_Constructor) {
    str8_list_pushf(scratch.arena, &list, "Constructor");
  }
  if (x & CV_FunctionAttrib_ConstructorVBase) {
    str8_list_pushf(scratch.arena, &list, "ConstructorVBase");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_export_flags(Arena *arena, CV_ExportFlags x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_ExportFlag_Constant) {
    str8_list_pushf(scratch.arena, &list, "Constant");
  }
  if (x & CV_ExportFlag_Data) {
    str8_list_pushf(scratch.arena, &list, "Data");
  }
  if (x & CV_ExportFlag_Private) {
    str8_list_pushf(scratch.arena, &list, "Private");
  }
  if (x & CV_ExportFlag_NoName) {
    str8_list_pushf(scratch.arena, &list, "NoName");
  }
  if (x & CV_ExportFlag_Ordinal) {
    str8_list_pushf(scratch.arena, &list, "Ordinal");
  }
  if (x & CV_ExportFlag_Forwarder) {
    str8_list_pushf(scratch.arena, &list, "Forwarder");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_sepcode(Arena *arena, CV_SepcodeFlags x)
{
  Temp scratch = scratch_begin(&arena,1);
  String8List list = {0};
  if (x & CV_SepcodeFlag_IsLexicalScope) {
    str8_list_pushf(scratch.arena, &list, "IsLexicalScope");
  }
  if (x & CV_SepcodeFlag_ReturnsToParent) {
    str8_list_pushf(scratch.arena, &list, "ReturnsToParent");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_pub32_flags(Arena *arena, CV_Pub32Flags x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_Pub32Flag_Code) {
    str8_list_pushf(scratch.arena, &list, "Code");
  }
  if (x & CV_Pub32Flag_Function) {
    str8_list_pushf(scratch.arena, &list, "Function");
  }
  if (x & CV_Pub32Flag_ManagedCode) {
    str8_list_pushf(scratch.arena, &list, "ManagedCode");
  }
  if (x & CV_Pub32Flag_MSIL) {
    str8_list_pushf(scratch.arena, &list, "MSIL");
  }
  String8 result = str8_list_join(scratch.arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_generic_flags(Arena *arena, CV_GenericFlags x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_GenericFlags_CSTYLE) {
    str8_list_pushf(scratch.arena, &list, "CSTYLE");
  }
  if (x & CV_GenericFlags_RSCLEAN) {
    str8_list_pushf(scratch.arena, &list, "RSCLEAN");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_frame_proc_flags(Arena *arena, CV_FrameprocFlags x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_FrameprocFlag_UsesAlloca) {
    str8_list_pushf(scratch.arena, &list, "UsesAlloca");
  }
  if (x & CV_FrameprocFlag_UsesSetJmp) {
    str8_list_pushf(scratch.arena, &list, "UsesSetJmp");
  }
  if (x & CV_FrameprocFlag_UsesLongJmp) {
    str8_list_pushf(scratch.arena, &list, "UsesLongJmp");
  }
  if (x & CV_FrameprocFlag_UsesInlAsm) {
    str8_list_pushf(scratch.arena, &list, "UsesInlAsm");
  }
  if (x & CV_FrameprocFlag_UsesEH) {
    str8_list_pushf(scratch.arena, &list, "UsesEH");
  }
  if (x & CV_FrameprocFlag_Inline) {
    str8_list_pushf(scratch.arena, &list, "Inline");
  }
  if (x & CV_FrameprocFlag_HasSEH) {
    str8_list_pushf(scratch.arena, &list, "HasSEH");
  }
  if (x & CV_FrameprocFlag_Naked) {
    str8_list_pushf(scratch.arena, &list, "Naked");
  }
  if (x & CV_FrameprocFlag_HasSecurityChecks) {
    str8_list_pushf(scratch.arena, &list, "HasSecurityChecks");
  }
  if (x & CV_FrameprocFlag_AsyncEH) {
    str8_list_pushf(scratch.arena, &list, "AsyncEH");
  }
  if (x & CV_FrameprocFlag_GSNoStackOrdering) {
    str8_list_pushf(scratch.arena, &list, "GSNoStackOrdering");
  }
  if (x & CV_FrameprocFlag_WasInlined) {
    str8_list_pushf(scratch.arena, &list, "WasInlined");
  }
  if (x & CV_FrameprocFlag_GSCheck) {
    str8_list_pushf(scratch.arena, &list, "GSCheck");
  }
  if (x & CV_FrameprocFlag_SafeBuffers) {
    str8_list_pushf(scratch.arena, &list, "SafeBuffers");
  }
  if (x & CV_FrameprocFlag_PogoOn) {
    str8_list_pushf(scratch.arena, &list, "PogoOn");
  }
  if (x & CV_FrameprocFlag_PogoCountsValid) {
    str8_list_pushf(scratch.arena, &list, "PogoCountsValid");
  }
  if (x & CV_FrameprocFlag_OptSpeed) {
    str8_list_pushf(scratch.arena, &list, "OptSpeed");
  }
  if (x & CV_FrameprocFlag_HasCFG) {
    str8_list_pushf(scratch.arena, &list, "HasCFG");
  }
  if (x & CV_FrameprocFlag_HasCFW) {
    str8_list_pushf(scratch.arena, &list, "HasCFW");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_type_props(Arena *arena, CV_TypeProps x)
{
  Temp scratch = scratch_begin(&arena, 1);
  U32 hfa  = CV_TypeProps_Extract_HFA(x);
  U32 mcom = CV_TypeProps_Extract_MOCOM(x);

  String8 flags_str;
  {
    String8List list = {0};
    if (x & CV_TypeProp_Packed) {
      str8_list_pushf(scratch.arena, &list, "Packed");
    }
    if (x & CV_TypeProp_HasConstructorsDestructors) {
      str8_list_pushf(scratch.arena, &list, "HasConstructorsDestructors");
    }
    if (x & CV_TypeProp_OverloadedOperators) {
      str8_list_pushf(scratch.arena, &list, "OverloadedOperators");
    }
    if (x & CV_TypeProp_IsNested) {
      str8_list_pushf(scratch.arena, &list, "IsNested");
    }
    if (x & CV_TypeProp_ContainsNested) {
      str8_list_pushf(scratch.arena, &list, "ContainsNested");
    }
    if (x & CV_TypeProp_OverloadedAssignment) {
      str8_list_pushf(scratch.arena, &list, "OverloadedAssignment");
    }
    if (x & CV_TypeProp_OverloadedCasting) {
      str8_list_pushf(scratch.arena, &list, "OverloadedCasting");
    }
    if (x & CV_TypeProp_FwdRef) {
      str8_list_pushf(scratch.arena, &list, "FwdRef");
    }
    if (x & CV_TypeProp_Scoped) {
      str8_list_pushf(scratch.arena, &list, "Scoped");
    }
    if (x & CV_TypeProp_HasUniqueName) {
      str8_list_pushf(scratch.arena, &list, "HasUniqueName");
    }
    if (x & CV_TypeProp_Sealed) {
      str8_list_pushf(scratch.arena, &list, "Sealed");
    }
    flags_str = str8_list_join(scratch.arena, &list, &(StringJoin){.sep=str8_lit(", ") });
  }
  
  String8 hfa_str  = cv_string_from_hfa(hfa);
  String8 mcom_str = cv_string_from_mcom(mcom);
  String8 result   = push_str8f(arena, "flags = %S, HFA = %S, MCOM = %S", flags_str, hfa_str, mcom_str);

  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_local_flags(Arena *arena, CV_LocalFlags x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_LocalFlag_Param) {
    str8_list_pushf(scratch.arena, &list, "Param");
  }
  if (x & CV_LocalFlag_AddrTaken) {
    str8_list_pushf(scratch.arena, &list, "AddrTaken");
  }
  if (x & CV_LocalFlag_Compgen) {
    str8_list_pushf(scratch.arena, &list, "Compgen");
  }
  if (x & CV_LocalFlag_Aggregate) {
    str8_list_pushf(scratch.arena, &list, "Aggregate");
  }
  if (x & CV_LocalFlag_PartOfAggregate) {
    str8_list_pushf(scratch.arena, &list, "PartOfAggregate");
  }
  if (x & CV_LocalFlag_Aliased) {
    str8_list_pushf(scratch.arena, &list, "Aliased");
  }
  if (x & CV_LocalFlag_Alias) {
    str8_list_pushf(scratch.arena, &list, "Alias");
  }
  if (x & CV_LocalFlag_Retval) {
    str8_list_pushf(scratch.arena, &list, "Retval");
  }
  if (x & CV_LocalFlag_OptOut) {
    str8_list_pushf(scratch.arena, &list, "OptOut");
  }
  if (x & CV_LocalFlag_Global) {
    str8_list_pushf(scratch.arena, &list, "Global");
  }
  if (x & CV_LocalFlag_Static) {
    str8_list_pushf(scratch.arena, &list, "Static");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_proc_flags(Arena *arena, CV_ProcFlags x)
{
  Temp scratch = scratch_begin(&arena, 1);
  String8List list = {0};
  if (x & CV_ProcFlag_NoFPO) {
    str8_list_pushf(scratch.arena, &list, "NoFPO");
  }
  if (x & CV_ProcFlag_IntReturn) {
    str8_list_pushf(scratch.arena, &list, "IntReturn");
  }
  if (x & CV_ProcFlag_FarReturn) {
    str8_list_pushf(scratch.arena, &list, "FarReturn");
  }
  if (x & CV_ProcFlag_NeverReturn) {
    str8_list_pushf(scratch.arena, &list, "NeverReturn");
  }
  if (x & CV_ProcFlag_NotReached) {
    str8_list_pushf(scratch.arena, &list, "NotReached");
  }
  if (x & CV_ProcFlag_CustomCall) {
    str8_list_pushf(scratch.arena, &list, "CustomCall");
  }
  if (x & CV_ProcFlag_NoInline) {
    str8_list_pushf(scratch.arena, &list, "NoInline");
  }
  if (x & CV_ProcFlag_OptDbgInfo) {
    str8_list_pushf(scratch.arena, &list, "OptDbgInfo");
  }
  String8 result = str8_list_join(arena, &list, &(StringJoin){.sep=str8_lit(", ")});
  temp_end(scratch);
  return result;
}

internal String8
cv_string_from_range_attribs(Arena *arena, CV_RangeAttribs x)
{ (void)arena;
  String8 result = str8_lit("None");
  if (x == CV_RangeAttrib_Maybe) {
    result = str8_lit("Maybe");
  }
  return result;
}

internal String8
cv_string_from_defrange_register_rel_flags(Arena *arena, CV_DefrangeRegisterRelFlags x)
{ (void)arena;
  String8 result = str8_lit("None");
  if (x == CV_DefrangeRegisterRelFlag_SpilledOutUDTMember) {
    result = str8_lit("SpilledOutUDTMember");
  }
  return result;
}

internal String8
cv_string_from_field_attribs(Arena *arena, CV_FieldAttribs attribs)
{
  Temp scratch = scratch_begin(&arena, 1);

  U32 access = CV_FieldAttribs_Extract_ACCESS(attribs);
  U32 mprop  = CV_FieldAttribs_Extract_MPROP(attribs);

  String8 attribs_str;
  {
    String8List list = {0};
    if (attribs & CV_FieldAttrib_Pseudo) {
      str8_list_pushf(scratch.arena, &list, "Pseudo");
    }
    if (attribs & CV_FieldAttrib_NoInherit) {
      str8_list_pushf(scratch.arena, &list, "NoInherit");
    }
    if (attribs & CV_FieldAttrib_NoConstruct) {
      str8_list_pushf(scratch.arena, &list, "NoConstruct");
    }
    if (attribs & CV_FieldAttrib_CompilerGenated) {
      str8_list_pushf(scratch.arena, &list, "CompilerGenerated");
    }
    if (attribs & CV_FieldAttrib_Sealed) {
      str8_list_pushf(scratch.arena, &list, "Sealed");
    }
    attribs_str = str8_list_join(scratch.arena, &list, &(StringJoin){.sep=", " });
  }

  String8 access_str = cv_string_from_member_access(access);
  String8 mprop_str  = cv_string_from_method_prop(mprop);
  String8 result     = push_str8f(arena, "flags = %S, access = %S, method prop = %S", attribs_str, access_str, mprop_str);

  scratch_end(scratch);
  return result;
}

internal String8
cv_string_from_itype(Arena *arena, CV_TypeIndex itype)
{
  String8 result = push_str8f(arena, "%x", itype);
  return result;
}

internal String8
cv_string_from_itemid(Arena *arena, CV_ItemId itemid)
{
  String8 result = push_str8f(arena, "%x", itemid);
  return result;
}

internal String8
cv_string_from_reg_off(Arena *arena, CV_Arch arch, U32 reg, U32 off)
{
  return push_str8f(arena, "%S+%x", cv_string_from_reg_id(arch, reg), off);
}

internal String8
cv_string_from_symbol_type(Arena *arena, CV_SymKind symbol_type)
{
  String8 str    = cv_string_from_sym_kind(symbol_type);
  String8 result = push_str8f(arena, "S_%S", str);
  return result;
}

internal String8
cv_string_from_symbol_kind(Arena *arena, CV_SymKind kind)
{
  String8 str    = cv_string_from_sym_kind(kind);
  String8 result = push_str8f(arena, "S_%S", str);
  return result;
}

internal String8
cv_string_from_leaf_name(Arena *arena, U32 leaf_type)
{
  String8 str    = cv_string_from_leaf_kind(leaf_type);
  String8 result = push_str8f(arena, "LF_%S", str);
  return result;
}

internal String8 
cv_string_sec_off(Arena *arena, U32 sec, U32 off)
{
  return push_str8f(arena, "%04x:%08x", sec, off);
}
