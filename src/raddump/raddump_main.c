// Copyright (c) 2024 Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#define BUILD_CONSOLE_INTERFACE 1
#define BUILD_TITLE "Epic Games Tools (R) RAD Dumper"

////////////////////////////////

#include "linker/base_ext/base_blake3.h"
#include "linker/base_ext/base_blake3.c"
#include "third_party/xxHash/xxhash.c"
#include "third_party/xxHash/xxhash.h"
#include "third_party/radsort/radsort.h"
#include "third_party/md5/md5.c"
#include "third_party/md5/md5.h"
#include "third_party/zydis/zydis.h"
#include "third_party/zydis/zydis.c"
#include "third_party/rad_lzb_simple/rad_lzb_simple.h"
#include "third_party/rad_lzb_simple/rad_lzb_simple.c"

////////////////////////////////

#include "base/base_inc.h"
#include "os/os_inc.h"
#include "async/async.h"
#include "rdi_format/rdi_format_local.h"
#include "rdi_make/rdi_make_local.h"
#include "path/path.h"
#include "coff/coff.h"
#include "coff/coff_enum.h"
#include "pe/pe.h"
#include "msvc_crt/msvc_crt.h"
#include "msvc_crt/msvc_crt_enum.h"
#include "codeview/codeview.h"
#include "codeview/codeview_parse.h"
#include "codeview/codeview_enum.h"
#include "msf/msf.h"
#include "msf/msf_parse.h"
#include "pdb/pdb.h"
#include "pdb/pdb_parse.h"
#include "rdi_from_pdb/rdi_from_pdb.h"
#include "dwarf/dwarf.h"
#include "dwarf/dwarf_parse.h"
#include "dwarf/dwarf_expr.h"
#include "dwarf/dwarf_unwind.h"
#include "dwarf/dwarf_enum.h"

#include "base/base_inc.c"
#include "os/os_inc.c"
#include "async/async.c"
#include "rdi_format/rdi_format_local.c"
#include "rdi_make/rdi_make_local.c"
#include "path/path.c"
#include "coff/coff.c"
#include "coff/coff_enum.c"
#include "pe/pe.c"
#include "msvc_crt/msvc_crt.c"
#include "msvc_crt/msvc_crt_enum.c"
#include "codeview/codeview.c"
#include "codeview/codeview_parse.c"
#include "codeview/codeview_enum.c"
#include "msf/msf.c"
#include "msf/msf_parse.c"
#include "pdb/pdb.c"
#include "pdb/pdb_parse.c"
#include "rdi_from_pdb/rdi_from_pdb.c"
#include "dwarf/dwarf.c"
#include "dwarf/dwarf_parse.c"
#include "dwarf/dwarf_expr.c"
#include "dwarf/dwarf_unwind.c"
#include "dwarf/dwarf_enum.c"
 
#include "linker/base_ext/base_inc.h"
#include "linker/base_ext/base_inc.c"
#include "linker/path_ext/path.h"
#include "linker/path_ext/path.c"
#include "linker/thread_pool/thread_pool.h"
#include "linker/thread_pool/thread_pool.c"
#include "linker/codeview_ext/codeview.h"
#include "linker/codeview_ext/codeview.c"
#include "linker/hash_table.h"
#include "linker/hash_table.c"

#include "raddump/raddump.h"
#include "raddump/raddump.c"

////////////////////////////////

global read_only struct
{
  RD_Option opt;
  char     *name;
  char     *help;
} g_rd_dump_option_map[] = {
  { RD_Option_Help,             "help",                "Print help and exit"                                                  },
  { RD_Option_Version,          "version",             "Print version and exit"                                               },
  { RD_Option_Headers,          "headers",             "Dump DOS header, file header, optional header, and/or archive header" },
  { RD_Option_Sections,         "sections",            "Dump section headers as table"                                        },
  { RD_Option_Rawdata,          "rawdata",             "Dump raw section data"                                                },
  { RD_Option_Codeview,         "cv",                  "Dump CodeView"                                                        },
  { RD_Option_Disasm,           "disasm",              "Disassemble code sections"                                            },
  { RD_Option_Symbols,          "symtab",              "Dump COFF symbol table"                                               },
  { RD_Option_Relocs,           "relocs",              "Dump relocations"                                                     },
  { RD_Option_Exceptions,       "exceptions",          "Dump exceptions"                                                      },
  { RD_Option_Tls,              "tls",                 "Dump Thread Local Storage directory"                                  },
  { RD_Option_Debug,            "debug",               "Dump debug directory"                                                 },
  { RD_Option_Imports,          "imports",             "Dump import table"                                                    },
  { RD_Option_Exports,          "exports",             "Dump export table"                                                    },
  { RD_Option_LoadConfig,       "loadconfig",          "Dump load config"                                                     },
  { RD_Option_Resources,        "resources",           "Dump resource directory"                                              },
  { RD_Option_LongNames,        "longnames",           "Dump archive long names"                                              },
  { RD_Option_DebugInfo,        "debug_info",          "Dump .debug_info"                                                     },
  { RD_Option_DebugAbbrev,      "debug_abbrev",        "Dump .debug_abbrev"                                                   },
  { RD_Option_DebugLine,        "debug_line",          "Dump .debug_line"                                                     },
  { RD_Option_DebugStr,         "debug_str",           "Dump .debug_str"                                                      },
  { RD_Option_DebugLoc,         "debug_loc",           "Dump .debug_loc"                                                      },
  { RD_Option_DebugRanges,      "debug_ranges",        "Dump .debug_ranges"                                                   },
  { RD_Option_DebugARanges,     "debug_aranges",       "Dump .debug_aranges"                                                  },
  { RD_Option_DebugAddr,        "debug_addr",          "Dump .debug_addr"                                                     },
  { RD_Option_DebugLocLists,    "debug_loclists",      "Dump .debug_loclists"                                                 },
  { RD_Option_DebugRngLists,    "debug_rnglists",      "Dump .debug_rnglists"                                                 },
  { RD_Option_DebugPubNames,    "debug_pubnames",      "Dump .debug_pubnames"                                                 },
  { RD_Option_DebugPubTypes,    "debug_pubtypes",      "Dump .debug_putypes"                                                  },
  { RD_Option_DebugLineStr,     "debug_linestr",       "Dump .debug_linestr"                                                  },
  { RD_Option_DebugStrOffsets,  "debug_stroffsets",    "Dump .debug_stroffsets"                                               },
  { RD_Option_Dwarf,            "dwarf",               "Dump all DWARF sections"                                              },
  { RD_Option_RelaxDwarfParser, "relax_dwarf_parser",  "Relaxes version requirement on attribute and form encodings"          },
  { RD_Option_NoRdi,            "nordi",               "Don't load RAD Debug Info"                                            },

  { RD_Option_Help,             "h",                   "Alias for -help"                                                      },
  { RD_Option_Version,          "v",                   "Alias for -version"                                                   },
  { RD_Option_Sections,         "s",                   "Alias for -sections"                                                  },
  { RD_Option_Exceptions,       "e",                   "Alias for -exceptions"                                                },
  { RD_Option_Imports,          "i",                   "Alias for -imports"                                                   },
  { RD_Option_Exports,          "x",                   "Alias for -exports"                                                   },
  { RD_Option_LoadConfig,       "l",                   "Alias for -loadconifg"                                                },
  { RD_Option_Resources,        "c",                   "Alias for -resources"                                                 },
  { RD_Option_Relocs,           "r",                   "Alias for -relocs"                                                    },
};

internal void
entry_point(CmdLine *cmdline)
{
  Arena *arena = arena_alloc();

  // make indent
  String8List *out = push_array(arena, String8List, 1);
  String8      indent;
  {
    U64 indent_buffer_size = RD_INDENT_WIDTH * RD_INDENT_MAX;
    U8 *indent_buffer      = push_array(arena, U8, indent_buffer_size);
    MemorySet(indent_buffer, ' ', indent_buffer_size);
    indent = str8(indent_buffer, 0);
  }

  // parse options
  RD_Option opts = 0;
  {
    for (CmdLineOpt *cmd = cmdline->options.first; cmd != 0; cmd = cmd->next) {
      RD_Option opt = 0;
      for (U64 opt_idx = 0; opt_idx < ArrayCount(g_rd_dump_option_map); ++opt_idx) {
        String8 opt_name = str8_cstring(g_rd_dump_option_map[opt_idx].name);
        if (str8_match(cmd->string, opt_name, StringMatchFlag_CaseInsensitive)) {
          opt = g_rd_dump_option_map[opt_idx].opt;
          break;
        } else if (str8_match(cmd->string, str8_lit("all"), StringMatchFlag_CaseInsensitive)) {
          opt = ~0ull & ~(RD_Option_Help|RD_Option_Version);
          break;
        }
      }

      if (opt == 0) {
        rd_errorf("Unknown argument: \"%S\"", cmd->string);
        goto exit;
      }

      opts |= opt;
    }
  }

  // print help
  if (opts & RD_Option_Help) {
    int longest_cmd_switch = 0;
    for (U64 opt_idx = 0; opt_idx < ArrayCount(g_rd_dump_option_map); ++opt_idx) {
      longest_cmd_switch = Max(longest_cmd_switch, strlen(g_rd_dump_option_map[opt_idx].name));
    }
    rd_printf(BUILD_TITLE_STRING_LITERAL);
    rd_newline();
    rd_printf("# Help");
    rd_indent();
    for (U64 opt_idx = 0; opt_idx < ArrayCount(g_rd_dump_option_map); ++opt_idx) {
      char *name = g_rd_dump_option_map[opt_idx].name;
      char *help = g_rd_dump_option_map[opt_idx].help;
      int indent_size = longest_cmd_switch - strlen(name) + 1;
      rd_printf("-%s%.*s%s", g_rd_dump_option_map[opt_idx].name, indent_size, indent.str, g_rd_dump_option_map[opt_idx].help);
    }
    rd_unindent();
    goto exit;
  }

  // print version
  if (opts & RD_Option_Version) {
    rd_printf(BUILD_TITLE_STRING_LITERAL);
    rd_printf("\traddump <options> <inputs>");
    goto exit;
  }

  // input check
  if (cmdline->inputs.node_count == 0) {
    rd_errorf("No input file specified");
    goto exit;
  } else if (cmdline->inputs.node_count > 1) {
    rd_errorf("Too many inputs specified, expected one");
    goto exit;
  }

  // read input
  String8 file_path = str8_list_first(&cmdline->inputs);
  String8 raw_data  = os_data_from_file_path(arena, file_path);

  // is read ok?
  if (raw_data.size == 0) {
    rd_errorf("Unable to read input file \"%S\"", file_path);
    goto exit;
  }

  // format input
  rd_format_preamble(arena, out, indent, file_path, raw_data);
  if (coff_is_archive(raw_data) || coff_is_thin_archive(raw_data)) {
    coff_print_archive(arena, out, indent, raw_data, opts);
  } else if (coff_is_big_obj(raw_data)) {
    coff_print_big_obj(arena, out, indent, raw_data, opts);
  } else if (coff_is_obj(raw_data)) {
    coff_print_obj(arena, out, indent, raw_data, opts);
  } else if (rd_is_pe(raw_data)) {
    RDI_Parsed *rdi = 0;
    if (!(opts & RD_Option_NoRdi)) {
      rdi = rd_rdi_from_pe(arena, file_path, raw_data);
    }
    pe_print(arena, out, indent, raw_data, opts, rdi);
  } else if (pe_is_res(raw_data)) {
    //tool_out_coff_res(stdout, file_data);
  }
  
exit:;
  // print formatted string
  String8 out_string = str8_list_join(arena, out, &(StringJoin){ .sep = str8_lit("\n"),});
  fprintf(stdout, "%.*s", str8_varg(out_string));

  arena_release(arena);
}

