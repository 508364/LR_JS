/*
 * LR_JS — Bytecode VM (modular stub)
 */
#include "lr_bytecode.h"
#include "lr_interp.h"
#include "lr_jit.h"
#include "lr_ast.h"
#include "lr_platform.h"

static int bc_env_debug_call    = -1;
static int bc_env_debug_jitcall = -1;
static int bc_env_debug_inline  = -1;
#include "lr_bytecode_cache.h"
#include "lr_bytecode_emit.h"
#include "lr_bytecode_compile.h"
#include "lr_bytecode_value.h"
#include "lr_bytecode_exec.h"
#include "lr_bytecode_serialize.h"
#include "lr_bytecode_disasm.h"
