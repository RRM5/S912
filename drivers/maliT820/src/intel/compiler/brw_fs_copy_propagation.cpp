/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/** @file brw_fs_copy_propagation.cpp
 *
 * Support for global copy propagation in two passes: A local pass that does
 * intra-block copy (and constant) propagation, and a global pass that uses
 * dataflow analysis on the copies available at the end of each block to re-do
 * local copy propagation with more copies available.
 *
 * See Muchnick's Advanced Compiler Design and Implementation, section
 * 12.5 (p356).
 */

#define ACP_HASH_SIZE 16

#include "util/bitset.h"
#include "brw_fs.h"
#include "brw_fs_live_variables.h"
#include "brw_cfg.h"
#include "brw_eu.h"

using namespace brw;

namespace { /* avoid conflict with opt_copy_propagation_elements */
struct acp_entry : public exec_node {
   fs_reg dst;
   fs_reg src;
   uint8_t size_written;
   uint8_t size_read;
   enum opcode opcode;
   bool saturate;
};

struct block_data {
   /**
    * Which entries in the fs_copy_prop_dataflow acp table are live at the
    * start of this block.  This is the useful output of the analysis, since
    * it lets us plug those into the local copy propagation on the second
    * pass.
    */
   BITSET_WORD *livein;

   /**
    * Which entries in the fs_copy_prop_dataflow acp table are live at the end
    * of this block.  This is done in initial setup from the per-block acps
    * returned by the first local copy prop pass.
    */
   BITSET_WORD *liveout;

   /**
    * Which entries in the fs_copy_prop_dataflow acp table are generated by
    * instructions in this block which reach the end of the block without
    * being killed.
    */
   BITSET_WORD *copy;

   /**
    * Which entries in the fs_copy_prop_dataflow acp table are killed over the
    * course of this block.
    */
   BITSET_WORD *kill;

   /**
    * Which entries in the fs_copy_prop_dataflow acp table are guaranteed to
    * have a fully uninitialized destination at the end of this block.
    */
   BITSET_WORD *undef;
};

class fs_copy_prop_dataflow
{
public:
   fs_copy_prop_dataflow(void *mem_ctx, cfg_t *cfg,
                         const fs_live_variables *live,
                         exec_list *out_acp[ACP_HASH_SIZE]);

   void setup_initial_values();
   void run();

   void dump_block_data() const UNUSED;

   void *mem_ctx;
   cfg_t *cfg;
   const fs_live_variables *live;

   acp_entry **acp;
   int num_acp;
   int bitset_words;

  struct block_data *bd;
};
} /* anonymous namespace */

fs_copy_prop_dataflow::fs_copy_prop_dataflow(void *mem_ctx, cfg_t *cfg,
                                             const fs_live_variables *live,
                                             exec_list *out_acp[ACP_HASH_SIZE])
   : mem_ctx(mem_ctx), cfg(cfg), live(live)
{
   bd = rzalloc_array(mem_ctx, struct block_data, cfg->num_blocks);

   num_acp = 0;
   foreach_block (block, cfg) {
      for (int i = 0; i < ACP_HASH_SIZE; i++) {
         num_acp += out_acp[block->num][i].length();
      }
   }

   acp = rzalloc_array(mem_ctx, struct acp_entry *, num_acp);

   bitset_words = BITSET_WORDS(num_acp);

   int next_acp = 0;
   foreach_block (block, cfg) {
      bd[block->num].livein = rzalloc_array(bd, BITSET_WORD, bitset_words);
      bd[block->num].liveout = rzalloc_array(bd, BITSET_WORD, bitset_words);
      bd[block->num].copy = rzalloc_array(bd, BITSET_WORD, bitset_words);
      bd[block->num].kill = rzalloc_array(bd, BITSET_WORD, bitset_words);
      bd[block->num].undef = rzalloc_array(bd, BITSET_WORD, bitset_words);

      for (int i = 0; i < ACP_HASH_SIZE; i++) {
         foreach_in_list(acp_entry, entry, &out_acp[block->num][i]) {
            acp[next_acp] = entry;

            /* opt_copy_propagation_local populates out_acp with copies created
             * in a block which are still live at the end of the block.  This
             * is exactly what we want in the COPY set.
             */
            BITSET_SET(bd[block->num].copy, next_acp);

            next_acp++;
         }
      }
   }

   assert(next_acp == num_acp);

   setup_initial_values();
   run();
}

/**
 * Set up initial values for each of the data flow sets, prior to running
 * the fixed-point algorithm.
 */
void
fs_copy_prop_dataflow::setup_initial_values()
{
   /* Initialize the COPY and KILL sets. */
   foreach_block (block, cfg) {
      foreach_inst_in_block(fs_inst, inst, block) {
         if (inst->dst.file != VGRF)
            continue;

         /* Mark ACP entries which are killed by this instruction. */
         for (int i = 0; i < num_acp; i++) {
            if (regions_overlap(inst->dst, inst->size_written,
                                acp[i]->dst, acp[i]->size_written) ||
                regions_overlap(inst->dst, inst->size_written,
                                acp[i]->src, acp[i]->size_read)) {
               BITSET_SET(bd[block->num].kill, i);
            }
         }
      }
   }

   /* Populate the initial values for the livein and liveout sets.  For the
    * block at the start of the program, livein = 0 and liveout = copy.
    * For the others, set liveout and livein to ~0 (the universal set).
    */
   foreach_block (block, cfg) {
      if (block->parents.is_empty()) {
         for (int i = 0; i < bitset_words; i++) {
            bd[block->num].livein[i] = 0u;
            bd[block->num].liveout[i] = bd[block->num].copy[i];
         }
      } else {
         for (int i = 0; i < bitset_words; i++) {
            bd[block->num].liveout[i] = ~0u;
            bd[block->num].livein[i] = ~0u;
         }
      }
   }

   /* Initialize the undef set. */
   foreach_block (block, cfg) {
      for (int i = 0; i < num_acp; i++) {
         BITSET_SET(bd[block->num].undef, i);
         for (unsigned off = 0; off < acp[i]->size_written; off += REG_SIZE) {
            if (BITSET_TEST(live->block_data[block->num].defout,
                            live->var_from_reg(byte_offset(acp[i]->dst, off))))
               BITSET_CLEAR(bd[block->num].undef, i);
         }
      }
   }
}

/**
 * Walk the set of instructions in the block, marking which entries in the acp
 * are killed by the block.
 */
void
fs_copy_prop_dataflow::run()
{
   bool progress;

   do {
      progress = false;

      foreach_block (block, cfg) {
         if (block->parents.is_empty())
            continue;

         for (int i = 0; i < bitset_words; i++) {
            const BITSET_WORD old_liveout = bd[block->num].liveout[i];
            BITSET_WORD livein_from_any_block = 0;

            /* Update livein for this block.  If a copy is live out of all
             * parent blocks, it's live coming in to this block.
             */
            bd[block->num].livein[i] = ~0u;
            foreach_list_typed(bblock_link, parent_link, link, &block->parents) {
               bblock_t *parent = parent_link->block;
               /* Consider ACP entries with a known-undefined destination to
                * be available from the parent.  This is valid because we're
                * free to set the undefined variable equal to the source of
                * the ACP entry without breaking the application's
                * expectations, since the variable is undefined.
                */
               bd[block->num].livein[i] &= (bd[parent->num].liveout[i] |
                                            bd[parent->num].undef[i]);
               livein_from_any_block |= bd[parent->num].liveout[i];
            }

            /* Limit to the set of ACP entries that can possibly be available
             * at the start of the block, since propagating from a variable
             * which is guaranteed to be undefined (rather than potentially
             * undefined for some dynamic control-flow paths) doesn't seem
             * particularly useful.
             */
            bd[block->num].livein[i] &= livein_from_any_block;

            /* Update liveout for this block. */
            bd[block->num].liveout[i] =
               bd[block->num].copy[i] | (bd[block->num].livein[i] &
                                         ~bd[block->num].kill[i]);

            if (old_liveout != bd[block->num].liveout[i])
               progress = true;
         }
      }
   } while (progress);
}

void
fs_copy_prop_dataflow::dump_block_data() const
{
   foreach_block (block, cfg) {
      fprintf(stderr, "Block %d [%d, %d] (parents ", block->num,
             block->start_ip, block->end_ip);
      foreach_list_typed(bblock_link, link, link, &block->parents) {
         bblock_t *parent = link->block;
         fprintf(stderr, "%d ", parent->num);
      }
      fprintf(stderr, "):\n");
      fprintf(stderr, "       livein = 0x");
      for (int i = 0; i < bitset_words; i++)
         fprintf(stderr, "%08x", bd[block->num].livein[i]);
      fprintf(stderr, ", liveout = 0x");
      for (int i = 0; i < bitset_words; i++)
         fprintf(stderr, "%08x", bd[block->num].liveout[i]);
      fprintf(stderr, ",\n       copy   = 0x");
      for (int i = 0; i < bitset_words; i++)
         fprintf(stderr, "%08x", bd[block->num].copy[i]);
      fprintf(stderr, ", kill    = 0x");
      for (int i = 0; i < bitset_words; i++)
         fprintf(stderr, "%08x", bd[block->num].kill[i]);
      fprintf(stderr, "\n");
   }
}

static bool
is_logic_op(enum opcode opcode)
{
   return (opcode == BRW_OPCODE_AND ||
           opcode == BRW_OPCODE_OR  ||
           opcode == BRW_OPCODE_XOR ||
           opcode == BRW_OPCODE_NOT);
}

static bool
can_take_stride(fs_inst *inst, unsigned arg, unsigned stride,
                const gen_device_info *devinfo)
{
   if (stride > 4)
      return false;

   /* Bail if the channels of the source need to be aligned to the byte offset
    * of the corresponding channel of the destination, and the provided stride
    * would break this restriction.
    */
   if (has_dst_aligned_region_restriction(devinfo, inst) &&
       !(type_sz(inst->src[arg].type) * stride ==
           type_sz(inst->dst.type) * inst->dst.stride ||
         stride == 0))
      return false;

   /* 3-source instructions can only be Align16, which restricts what strides
    * they can take. They can only take a stride of 1 (the usual case), or 0
    * with a special "repctrl" bit. But the repctrl bit doesn't work for
    * 64-bit datatypes, so if the source type is 64-bit then only a stride of
    * 1 is allowed. From the Broadwell PRM, Volume 7 "3D Media GPGPU", page
    * 944:
    *
    *    This is applicable to 32b datatypes and 16b datatype. 64b datatypes
    *    cannot use the replicate control.
    */
   if (inst->is_3src(devinfo)) {
      if (type_sz(inst->src[arg].type) > 4)
         return stride == 1;
      else
         return stride == 1 || stride == 0;
   }

   /* From the Broadwell PRM, Volume 2a "Command Reference - Instructions",
    * page 391 ("Extended Math Function"):
    *
    *     The following restrictions apply for align1 mode: Scalar source is
    *     supported. Source and destination horizontal stride must be the
    *     same.
    *
    * From the Haswell PRM Volume 2b "Command Reference - Instructions", page
    * 134 ("Extended Math Function"):
    *
    *    Scalar source is supported. Source and destination horizontal stride
    *    must be 1.
    *
    * and similar language exists for IVB and SNB. Pre-SNB, math instructions
    * are sends, so the sources are moved to MRF's and there are no
    * restrictions.
    */
   if (inst->is_math()) {
      if (devinfo->gen == 6 || devinfo->gen == 7) {
         assert(inst->dst.stride == 1);
         return stride == 1 || stride == 0;
      } else if (devinfo->gen >= 8) {
         return stride == inst->dst.stride || stride == 0;
      }
   }

   return true;
}

static bool
instruction_requires_packed_data(fs_inst *inst)
{
   switch (inst->opcode) {
   case FS_OPCODE_DDX_FINE:
   case FS_OPCODE_DDX_COARSE:
   case FS_OPCODE_DDY_FINE:
   case FS_OPCODE_DDY_COARSE:
      return true;
   default:
      return false;
   }
}

bool
fs_visitor::try_copy_propagate(fs_inst *inst, int arg, acp_entry *entry)
{
   if (inst->src[arg].file != VGRF)
      return false;

   if (entry->src.file == IMM)
      return false;
   assert(entry->src.file == VGRF || entry->src.file == UNIFORM ||
          entry->src.file == ATTR);

   if (entry->opcode == SHADER_OPCODE_LOAD_PAYLOAD &&
       inst->opcode == SHADER_OPCODE_LOAD_PAYLOAD)
      return false;

   assert(entry->dst.file == VGRF);
   if (inst->src[arg].nr != entry->dst.nr)
      return false;

   /* Bail if inst is reading a range that isn't contained in the range
    * that entry is writing.
    */
   if (!region_contained_in(inst->src[arg], inst->size_read(arg),
                            entry->dst, entry->size_written))
      return false;

   /* we can't generally copy-propagate UD negations because we
    * can end up accessing the resulting values as signed integers
    * instead. See also resolve_ud_negate() and comment in
    * fs_generator::generate_code.
    */
   if (entry->src.type == BRW_REGISTER_TYPE_UD &&
       entry->src.negate)
      return false;

   bool has_source_modifiers = entry->src.abs || entry->src.negate;

   if ((has_source_modifiers || entry->src.file == UNIFORM ||
        !entry->src.is_contiguous()) &&
       !inst->can_do_source_mods(devinfo))
      return false;

   if (has_source_modifiers &&
       inst->opcode == SHADER_OPCODE_GEN4_SCRATCH_WRITE)
      return false;

   /* Some instructions implemented in the generator backend, such as
    * derivatives, assume that their operands are packed so we can't
    * generally propagate strided regions to them.
    */
   if (instruction_requires_packed_data(inst) && entry->src.stride > 1)
      return false;

   /* Bail if the result of composing both strides would exceed the
    * hardware limit.
    */
   if (!can_take_stride(inst, arg, entry->src.stride * inst->src[arg].stride,
                        devinfo))
      return false;

   /* Bail if the instruction type is larger than the execution type of the
    * copy, what implies that each channel is reading multiple channels of the
    * destination of the copy, and simply replacing the sources would give a
    * program with different semantics.
    */
   if (type_sz(entry->dst.type) < type_sz(inst->src[arg].type))
      return false;

   /* Bail if the result of composing both strides cannot be expressed
    * as another stride. This avoids, for example, trying to transform
    * this:
    *
    *     MOV (8) rX<1>UD rY<0;1,0>UD
    *     FOO (8) ...     rX<8;8,1>UW
    *
    * into this:
    *
    *     FOO (8) ...     rY<0;1,0>UW
    *
    * Which would have different semantics.
    */
   if (entry->src.stride != 1 &&
       (inst->src[arg].stride *
        type_sz(inst->src[arg].type)) % type_sz(entry->src.type) != 0)
      return false;

   /* Since semantics of source modifiers are type-dependent we need to
    * ensure that the meaning of the instruction remains the same if we
    * change the type. If the sizes of the types are different the new
    * instruction will read a different amount of data than the original
    * and the semantics will always be different.
    */
   if (has_source_modifiers &&
       entry->dst.type != inst->src[arg].type &&
       (!inst->can_change_types() ||
        type_sz(entry->dst.type) != type_sz(inst->src[arg].type)))
      return false;

   if (devinfo->gen >= 8 && (entry->src.negate || entry->src.abs) &&
       is_logic_op(inst->opcode)) {
      return false;
   }

   if (entry->saturate) {
      switch(inst->opcode) {
      case BRW_OPCODE_SEL:
         if ((inst->conditional_mod != BRW_CONDITIONAL_GE &&
              inst->conditional_mod != BRW_CONDITIONAL_L) ||
             inst->src[1].file != IMM ||
             inst->src[1].f < 0.0 ||
             inst->src[1].f > 1.0) {
            return false;
         }
         break;
      default:
         return false;
      }
   }

   inst->src[arg].file = entry->src.file;
   inst->src[arg].nr = entry->src.nr;
   inst->src[arg].stride *= entry->src.stride;
   inst->saturate = inst->saturate || entry->saturate;

   /* Compute the offset of inst->src[arg] relative to entry->dst */
   const unsigned rel_offset = inst->src[arg].offset - entry->dst.offset;

   /* Compute the first component of the copy that the instruction is
    * reading, and the base byte offset within that component.
    */
   assert(entry->dst.offset % REG_SIZE == 0 && entry->dst.stride == 1);
   const unsigned component = rel_offset / type_sz(entry->dst.type);
   const unsigned suboffset = rel_offset % type_sz(entry->dst.type);

   /* Calculate the byte offset at the origin of the copy of the given
    * component and suboffset.
    */
   inst->src[arg].offset = suboffset +
      component * entry->src.stride * type_sz(entry->src.type) +
      entry->src.offset;

   if (has_source_modifiers) {
      if (entry->dst.type != inst->src[arg].type) {
         /* We are propagating source modifiers from a MOV with a different
          * type.  If we got here, then we can just change the source and
          * destination types of the instruction and keep going.
          */
         assert(inst->can_change_types());
         for (int i = 0; i < inst->sources; i++) {
            inst->src[i].type = entry->dst.type;
         }
         inst->dst.type = entry->dst.type;
      }

      if (!inst->src[arg].abs) {
         inst->src[arg].abs = entry->src.abs;
         inst->src[arg].negate ^= entry->src.negate;
      }
   }

   return true;
}


bool
fs_visitor::try_constant_propagate(fs_inst *inst, acp_entry *entry)
{
   bool progress = false;

   if (entry->src.file != IMM)
      return false;
   if (type_sz(entry->src.type) > 4)
      return false;
   if (entry->saturate)
      return false;

   for (int i = inst->sources - 1; i >= 0; i--) {
      if (inst->src[i].file != VGRF)
         continue;

      assert(entry->dst.file == VGRF);
      if (inst->src[i].nr != entry->dst.nr)
         continue;

      /* Bail if inst is reading a range that isn't contained in the range
       * that entry is writing.
       */
      if (!region_contained_in(inst->src[i], inst->size_read(i),
                               entry->dst, entry->size_written))
         continue;

      /* If the type sizes don't match each channel of the instruction is
       * either extracting a portion of the constant (which could be handled
       * with some effort but the code below doesn't) or reading multiple
       * channels of the source at once.
       */
      if (type_sz(inst->src[i].type) != type_sz(entry->dst.type))
         continue;

      fs_reg val = entry->src;
      val.type = inst->src[i].type;

      if (inst->src[i].abs) {
         if ((devinfo->gen >= 8 && is_logic_op(inst->opcode)) ||
             !brw_abs_immediate(val.type, &val.as_brw_reg())) {
            continue;
         }
      }

      if (inst->src[i].negate) {
         if ((devinfo->gen >= 8 && is_logic_op(inst->opcode)) ||
             !brw_negate_immediate(val.type, &val.as_brw_reg())) {
            continue;
         }
      }

      switch (inst->opcode) {
      case BRW_OPCODE_MOV:
      case SHADER_OPCODE_LOAD_PAYLOAD:
      case FS_OPCODE_PACK:
         inst->src[i] = val;
         progress = true;
         break;

      case SHADER_OPCODE_INT_QUOTIENT:
      case SHADER_OPCODE_INT_REMAINDER:
         /* FINISHME: Promote non-float constants and remove this. */
         if (devinfo->gen < 8)
            break;
         /* fallthrough */
      case SHADER_OPCODE_POW:
         /* Allow constant propagation into src1 (except on Gen 6 which
          * doesn't support scalar source math), and let constant combining
          * promote the constant on Gen < 8.
          */
         if (devinfo->gen == 6)
            break;
         /* fallthrough */
      case BRW_OPCODE_BFI1:
      case BRW_OPCODE_ASR:
      case BRW_OPCODE_SHL:
      case BRW_OPCODE_SHR:
      case BRW_OPCODE_SUBB:
         if (i == 1) {
            inst->src[i] = val;
            progress = true;
         }
         break;

      case BRW_OPCODE_MACH:
      case BRW_OPCODE_MUL:
      case SHADER_OPCODE_MULH:
      case BRW_OPCODE_ADD:
      case BRW_OPCODE_OR:
      case BRW_OPCODE_AND:
      case BRW_OPCODE_XOR:
      case BRW_OPCODE_ADDC:
         if (i == 1) {
            inst->src[i] = val;
            progress = true;
         } else if (i == 0 && inst->src[1].file != IMM) {
            /* Fit this constant in by commuting the operands.
             * Exception: we can't do this for 32-bit integer MUL/MACH
             * because it's asymmetric.
             *
             * The BSpec says for Broadwell that
             *
             *    "When multiplying DW x DW, the dst cannot be accumulator."
             *
             * Integer MUL with a non-accumulator destination will be lowered
             * by lower_integer_multiplication(), so don't restrict it.
             */
            if (((inst->opcode == BRW_OPCODE_MUL &&
                  inst->dst.is_accumulator()) ||
                 inst->opcode == BRW_OPCODE_MACH) &&
                (inst->src[1].type == BRW_REGISTER_TYPE_D ||
                 inst->src[1].type == BRW_REGISTER_TYPE_UD))
               break;
            inst->src[0] = inst->src[1];
            inst->src[1] = val;
            progress = true;
         }
         break;

      case BRW_OPCODE_CMP:
      case BRW_OPCODE_IF:
         if (i == 1) {
            inst->src[i] = val;
            progress = true;
         } else if (i == 0 && inst->src[1].file != IMM) {
            enum brw_conditional_mod new_cmod;

            new_cmod = brw_swap_cmod(inst->conditional_mod);
            if (new_cmod != BRW_CONDITIONAL_NONE) {
               /* Fit this constant in by swapping the operands and
                * flipping the test
                */
               inst->src[0] = inst->src[1];
               inst->src[1] = val;
               inst->conditional_mod = new_cmod;
               progress = true;
            }
         }
         break;

      case BRW_OPCODE_SEL:
         if (i == 1) {
            inst->src[i] = val;
            progress = true;
         } else if (i == 0 && inst->src[1].file != IMM) {
            inst->src[0] = inst->src[1];
            inst->src[1] = val;

            /* If this was predicated, flipping operands means
             * we also need to flip the predicate.
             */
            if (inst->conditional_mod == BRW_CONDITIONAL_NONE) {
               inst->predicate_inverse =
                  !inst->predicate_inverse;
            }
            progress = true;
         }
         break;

      case FS_OPCODE_FB_WRITE_LOGICAL:
         /* The stencil and omask sources of FS_OPCODE_FB_WRITE_LOGICAL are
          * bit-cast using a strided region so they cannot be immediates.
          */
         if (i != FB_WRITE_LOGICAL_SRC_SRC_STENCIL &&
             i != FB_WRITE_LOGICAL_SRC_OMASK) {
            inst->src[i] = val;
            progress = true;
         }
         break;

      case SHADER_OPCODE_TEX_LOGICAL:
      case SHADER_OPCODE_TXD_LOGICAL:
      case SHADER_OPCODE_TXF_LOGICAL:
      case SHADER_OPCODE_TXL_LOGICAL:
      case SHADER_OPCODE_TXS_LOGICAL:
      case FS_OPCODE_TXB_LOGICAL:
      case SHADER_OPCODE_TXF_CMS_LOGICAL:
      case SHADER_OPCODE_TXF_CMS_W_LOGICAL:
      case SHADER_OPCODE_TXF_UMS_LOGICAL:
      case SHADER_OPCODE_TXF_MCS_LOGICAL:
      case SHADER_OPCODE_LOD_LOGICAL:
      case SHADER_OPCODE_TG4_LOGICAL:
      case SHADER_OPCODE_TG4_OFFSET_LOGICAL:
      case SHADER_OPCODE_UNTYPED_ATOMIC_LOGICAL:
      case SHADER_OPCODE_UNTYPED_ATOMIC_FLOAT_LOGICAL:
      case SHADER_OPCODE_UNTYPED_SURFACE_READ_LOGICAL:
      case SHADER_OPCODE_UNTYPED_SURFACE_WRITE_LOGICAL:
      case SHADER_OPCODE_TYPED_ATOMIC_LOGICAL:
      case SHADER_OPCODE_TYPED_SURFACE_READ_LOGICAL:
      case SHADER_OPCODE_TYPED_SURFACE_WRITE_LOGICAL:
      case SHADER_OPCODE_BYTE_SCATTERED_WRITE_LOGICAL:
      case SHADER_OPCODE_BYTE_SCATTERED_READ_LOGICAL:
         inst->src[i] = val;
         progress = true;
         break;

      case FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD:
      case SHADER_OPCODE_BROADCAST:
         inst->src[i] = val;
         progress = true;
         break;

      case BRW_OPCODE_MAD:
      case BRW_OPCODE_LRP:
         inst->src[i] = val;
         progress = true;
         break;

      default:
         break;
      }
   }

   return progress;
}

static bool
can_propagate_from(fs_inst *inst)
{
   return (inst->opcode == BRW_OPCODE_MOV &&
           inst->dst.file == VGRF &&
           ((inst->src[0].file == VGRF &&
             !regions_overlap(inst->dst, inst->size_written,
                              inst->src[0], inst->size_read(0))) ||
            inst->src[0].file == ATTR ||
            inst->src[0].file == UNIFORM ||
            inst->src[0].file == IMM) &&
           inst->src[0].type == inst->dst.type &&
           !inst->is_partial_write());
}

/* Walks a basic block and does copy propagation on it using the acp
 * list.
 */
bool
fs_visitor::opt_copy_propagation_local(void *copy_prop_ctx, bblock_t *block,
                                       exec_list *acp)
{
   bool progress = false;

   foreach_inst_in_block(fs_inst, inst, block) {
      /* Try propagating into this instruction. */
      for (int i = 0; i < inst->sources; i++) {
         if (inst->src[i].file != VGRF)
            continue;

         foreach_in_list(acp_entry, entry, &acp[inst->src[i].nr % ACP_HASH_SIZE]) {
            if (try_constant_propagate(inst, entry))
               progress = true;
            else if (try_copy_propagate(inst, i, entry))
               progress = true;
         }
      }

      /* kill the destination from the ACP */
      if (inst->dst.file == VGRF) {
         foreach_in_list_safe(acp_entry, entry, &acp[inst->dst.nr % ACP_HASH_SIZE]) {
            if (regions_overlap(entry->dst, entry->size_written,
                                inst->dst, inst->size_written))
               entry->remove();
         }

         /* Oops, we only have the chaining hash based on the destination, not
          * the source, so walk across the entire table.
          */
         for (int i = 0; i < ACP_HASH_SIZE; i++) {
            foreach_in_list_safe(acp_entry, entry, &acp[i]) {
               /* Make sure we kill the entry if this instruction overwrites
                * _any_ of the registers that it reads
                */
               if (regions_overlap(entry->src, entry->size_read,
                                   inst->dst, inst->size_written))
                  entry->remove();
            }
	 }
      }

      /* If this instruction's source could potentially be folded into the
       * operand of another instruction, add it to the ACP.
       */
      if (can_propagate_from(inst)) {
         acp_entry *entry = ralloc(copy_prop_ctx, acp_entry);
         entry->dst = inst->dst;
         entry->src = inst->src[0];
         entry->size_written = inst->size_written;
         entry->size_read = inst->size_read(0);
         entry->opcode = inst->opcode;
         entry->saturate = inst->saturate;
         acp[entry->dst.nr % ACP_HASH_SIZE].push_tail(entry);
      } else if (inst->opcode == SHADER_OPCODE_LOAD_PAYLOAD &&
                 inst->dst.file == VGRF) {
         int offset = 0;
         for (int i = 0; i < inst->sources; i++) {
            int effective_width = i < inst->header_size ? 8 : inst->exec_size;
            assert(effective_width * type_sz(inst->src[i].type) % REG_SIZE == 0);
            const unsigned size_written = effective_width *
                                          type_sz(inst->src[i].type);
            if (inst->src[i].file == VGRF) {
               acp_entry *entry = rzalloc(copy_prop_ctx, acp_entry);
               entry->dst = byte_offset(inst->dst, offset);
               entry->src = inst->src[i];
               entry->size_written = size_written;
               entry->size_read = inst->size_read(i);
               entry->opcode = inst->opcode;
               if (!entry->dst.equals(inst->src[i])) {
                  acp[entry->dst.nr % ACP_HASH_SIZE].push_tail(entry);
               } else {
                  ralloc_free(entry);
               }
            }
            offset += size_written;
         }
      }
   }

   return progress;
}

bool
fs_visitor::opt_copy_propagation()
{
   bool progress = false;
   void *copy_prop_ctx = ralloc_context(NULL);
   exec_list *out_acp[cfg->num_blocks];

   for (int i = 0; i < cfg->num_blocks; i++)
      out_acp[i] = new exec_list [ACP_HASH_SIZE];

   calculate_live_intervals();

   /* First, walk through each block doing local copy propagation and getting
    * the set of copies available at the end of the block.
    */
   foreach_block (block, cfg) {
      progress = opt_copy_propagation_local(copy_prop_ctx, block,
                                            out_acp[block->num]) || progress;
   }

   /* Do dataflow analysis for those available copies. */
   fs_copy_prop_dataflow dataflow(copy_prop_ctx, cfg, live_intervals, out_acp);

   /* Next, re-run local copy propagation, this time with the set of copies
    * provided by the dataflow analysis available at the start of a block.
    */
   foreach_block (block, cfg) {
      exec_list in_acp[ACP_HASH_SIZE];

      for (int i = 0; i < dataflow.num_acp; i++) {
         if (BITSET_TEST(dataflow.bd[block->num].livein, i)) {
            struct acp_entry *entry = dataflow.acp[i];
            in_acp[entry->dst.nr % ACP_HASH_SIZE].push_tail(entry);
         }
      }

      progress = opt_copy_propagation_local(copy_prop_ctx, block, in_acp) ||
                 progress;
   }

   for (int i = 0; i < cfg->num_blocks; i++)
      delete [] out_acp[i];
   ralloc_free(copy_prop_ctx);

   if (progress)
      invalidate_live_intervals();

   return progress;
}
