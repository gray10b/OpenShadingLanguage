/*
Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <vector>
#include <string>
#include <cstdio>
#include <cmath>

#include <boost/foreach.hpp>

#include <OpenImageIO/timer.h>

#include "oslexec_pvt.h"
#include "oslops.h"
#include "../liboslcomp/oslcomp_pvt.h"
using namespace OSL;
using namespace OSL::pvt;



#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {
namespace pvt {   // OSL::pvt


// Search for a constant whose type and value match type and data[...].
// Return -1 if no matching const is found.
static int
find_constant (const SymbolVec &syms, const std::vector<int> &all_consts,
               const TypeSpec &type, const void *data)
{
    for (int i = 0;  i < (int)all_consts.size();  ++i) {
        const Symbol &s (syms[all_consts[i]]);
        ASSERT (s.symtype() == SymTypeConst);
        if (equivalent (s.typespec(), type) &&
              !memcmp (s.data(), data, s.typespec().simpletype().size())) {
            return all_consts[i];
        }
    }
    return -1;
}



// Search for a constant whose type and value match type and data[...],
// returning its index if one exists, or else creating a new constant
// and returning its index.  If copy is true, allocate new space and
// copy the data if no matching constant was found.
static int
add_constant (ShaderInstance &inst, std::vector<int> &all_consts,
              int &next_newconst, const TypeSpec &type, const void *data,
              bool copy=false)
{
    int ind = find_constant (inst.symbols(), all_consts, type, data);
    if (ind < 0) {
        Symbol newconst (ustring::format ("$newconst%d", next_newconst++),
                         type, SymTypeConst);
        if (copy) {
            void *newdata;
            TypeDesc t (type.simpletype());
            size_t n = t.aggregate * t.numelements();
            if (t.basetype == TypeDesc::INT)
                newdata = inst.shadingsys().alloc_int_constants (n);
            else if (t.basetype == TypeDesc::FLOAT)
                newdata = inst.shadingsys().alloc_float_constants (n);
            else if (t.basetype == TypeDesc::STRING)
                newdata = inst.shadingsys().alloc_string_constants (n);
            else { ASSERT (0 && "unsupported type for add_constant"); }
            memcpy (newdata, data, t.size());
            data = newdata;
        }
        newconst.data ((void *)data);
        ASSERT (inst.symbols().capacity() > inst.symbols().size() &&
                "we shouldn't have to realloc here");
        ind = (int) inst.symbols().size ();
        inst.symbols().push_back (newconst);
        all_consts.push_back (ind);
    }
    return ind;
}



// Turn the current op into a simple assignment.
void
turn_into_assign (Opcode &op, std::vector<int> &opargs, int newarg)
{
    static ustring kassign("assign");
    op.reset (kassign, OP_assign, 2);
    opargs[op.firstarg()+1] = newarg;
    op.argwriteonly (0);
    op.argread (1, true);
    op.argwrite (1, false);
}



// Insert instruction 'opname' with arguments 'args_to_add' into the 
// code at instruction 'opnum'.  The existing code and concatenated 
// argument lists can be found in code and opargs, respectively, and
// allsyms contains pointers to all symbols.  mainstart is a reference
// to the address where the 'main' shader begins, and may be modified
// if the new instruction is inserted before that point.
void
insert_code (ustring opname, OpImpl impl,
             std::vector<int> &args_to_add, int opnum, 
             OpcodeVec &code, std::vector<int> &opargs,
             SymbolPtrVec &allsyms, int &mainstart)
{
    ustring method = (opnum < (int)code.size()) ? code[opnum].method() : OSLCompilerImpl::main_method_name();
    Opcode op (opname, method, opargs.size(), args_to_add.size());
    op.implementation (impl);
    code.insert (code.begin()+opnum, op);
    opargs.insert (opargs.end(), args_to_add.begin(), args_to_add.end());
    if (opnum < mainstart)
        ++mainstart;

    // Unless we were inserting at the end, we may need to adjust
    // the jump addresses of other ops and the param init ranges.
    if (opnum < (int)code.size()-1) {
        // Adjust jump offsets
        for (size_t n = 0;  n < code.size();  ++n) {
            Opcode &c (code[n]);
            for (int j = 0; j < (int)Opcode::max_jumps && c.jump(j) >= 0; ++j) {
                if (c.jump(j) > opnum) {
                    c.jump(j) = c.jump(j) + 1;
                    // std::cerr << "Adjusting jump target at op " << n << "\n";
                }
            }
        }
        // Adjust param init ranges
        BOOST_FOREACH (Symbol *s, allsyms) {
            if (s->symtype() == SymTypeParam ||
                  s->symtype() == SymTypeOutputParam) {
                if (s->initbegin() > opnum)
                    s->initbegin (s->initbegin()+1);
                if (s->initend() > opnum)
                    s->initend (s->initend()+1);
            }
        }
    }
}



// Insert a 'useparam' instruction in front of instruction 'opnum', to
// reference the symbols in 'params'.
void
insert_useparam (OpcodeVec &code, size_t opnum, std::vector<int> &opargs,
                 SymbolPtrVec &allsyms, std::vector<int> &params_to_use,
                 int &mainstart)
{
    static ustring useparam("useparam");
    insert_code (useparam, OP_useparam, params_to_use, opnum,
                 code, opargs, allsyms, mainstart);

    // All ops are "read"
    code[opnum].argwrite (0, false);
    code[opnum].argread (0, true);
    if (opnum < code.size()-1) {
        // We have no parse node, but we set the new instruction's
        // "source" to the one of the statement right after.
        code[opnum].source (code[opnum+1].sourcefile(),
                            code[opnum+1].sourceline());
        // Set the method id to the same as the statement right after
        code[opnum].method (code[opnum+1].method());
    } else {
        // If there IS no "next" instruction, just call it main
        code[opnum].method (OSLCompilerImpl::main_method_name());
    }
}



// Add a 'useparam' before any op that reads parameters.  This is what
// tells the runtime that it needs to run the layer it came from, if
// not already done.
void
add_useparam (OpcodeVec &code, std::vector<int> &opargs,
              SymbolPtrVec &allsyms, int &mainstart)
{
    // Mark all symbols as un-initialized
    BOOST_FOREACH (Symbol *s, allsyms)
        s->initialized (false);

    if (mainstart < 0)
        mainstart = (int)code.size();

    // Take care of the output params right off the bat -- as soon as the
    // shader starts running 'main'.
    std::vector<int> outputparams;
    for (int i = 0;  i < (int)allsyms.size();  ++i) {
        Symbol *s = allsyms[i];
        if (s->symtype() == SymTypeOutputParam) {
            outputparams.push_back (i);
            s->initialized (true);
        }
    }
    if (outputparams.size())
        insert_useparam (code, mainstart, opargs, allsyms, outputparams,
                         mainstart);

    // Figure out which statements are inside conditional states
    std::vector<bool> in_conditional (code.size(), false);
    for (size_t opnum = 0;  opnum < code.size();  ++opnum) {
        // Find the farthest this instruction jumps to (-1 for instructions
        // that don't jump)
        int jumpend = code[opnum].farthest_jump();
        // Mark all instructions from here to there as inside conditionals
        for (int i = (int)opnum+1;  i < jumpend;  ++i)
            in_conditional[i] = true;
    }

    // Loop over all ops...
    for (int opnum = 0;  opnum < (int)code.size();  ++opnum) {
        Opcode &op (code[opnum]);  // handy ref to the op
        if (op.opname() == "useparam")
            continue;  // skip useparam ops themselves, if we hit one
        std::vector<int> params;   // list of params referenced by this op
        // For each argument...
        for (int a = 0;  a < op.nargs();  ++a) {
            int argind = op.firstarg() + a;
            SymbolPtr s = allsyms[opargs[argind]];
            DASSERT (s->dealias() == s);
            // If this arg is a param and is read, remember it
            if (s->symtype() != SymTypeParam && s->symtype() != SymTypeOutputParam)
                continue;  // skip non-params
            // skip if we've already 'usedparam'ed it unconditionally
            if (s->initialized() && opnum >= mainstart)
                continue;
            bool inside_init = (opnum >= s->initbegin() && opnum < s->initend());
            if (op.argread(a) || (op.argwrite(a) && !inside_init)) {
                // Don't add it more than once
                if (std::find (params.begin(), params.end(), opargs[argind]) == params.end()) {
                    params.push_back (opargs[argind]);
                    // mark as already initialized unconditionally, if we do
                    if (! in_conditional[opnum] && op.method() == OSLCompilerImpl::main_method_name())
                        s->initialized (true);
                }
            }
        }

        // If the arg we are examining read any params, insert a "useparam"
        // op whose arguments are the list of params we are about to use.
        if (params.size()) {
            insert_useparam (code, opnum, opargs, allsyms, params, mainstart);
            in_conditional.insert (in_conditional.begin()+opnum, false);
            // Skip the op we just added
            ++opnum;
        }
    }
    // Mark all symbols as un-initialized
    BOOST_FOREACH (Symbol *s, allsyms)
        s->initialized (false);
}



inline bool
equal_consts (const Symbol &A, const Symbol &B)
{
    return (&A == &B ||
            (equivalent (A.typespec(), B.typespec()) &&
             !memcmp (A.data(), B.data(), A.typespec().simpletype().size())));
}



inline bool
unequal_consts (const Symbol &A, const Symbol &B)
{
    return (equivalent (A.typespec(), B.typespec()) &&
            memcmp (A.data(), B.data(), A.typespec().simpletype().size()));
}



inline bool
is_zero (const Symbol &A)
{
    const TypeSpec &Atype (A.typespec());
    static Vec3 Vzero (0, 0, 0);
    return (Atype.is_float() && *(const float *)A.data() == 0) ||
        (Atype.is_int() && *(const int *)A.data() == 0) ||
        (Atype.is_triple() && *(const Vec3 *)A.data() == Vzero);
}



inline bool
is_one (const Symbol &A)
{
    const TypeSpec &Atype (A.typespec());
    static Vec3 Vone (1, 1, 1);
    static Matrix44 Mone (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    return (Atype.is_float() && *(const float *)A.data() == 1) ||
        (Atype.is_int() && *(const int *)A.data() == 1) ||
        (Atype.is_triple() && *(const Vec3 *)A.data() == Vone) ||
        (Atype.is_matrix() && *(const Matrix44 *)A.data() == Mone);
}



DECLFOLDER(constfold_add)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (A.is_constant()) {
        if (is_zero(A)) {
            // R = 0 + B  =>   R = B
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+2));
            return 1;
        }
    }
    if (B.is_constant()) {
        if (is_zero(B)) {
            // R = A + 0   =>   R = A
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+1));
            return 1;
        }
    }
    if (A.is_constant() && B.is_constant()) {
        if (A.typespec().is_float() && B.typespec().is_float()) {
            float result = *(float *)A.data() + *(float *)B.data();
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        } else if (A.typespec().is_triple() && B.typespec().is_triple()) {
            Vec3 result = *(Vec3 *)A.data() + *(Vec3 *)B.data();
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        }
    }
    return 0;
}



DECLFOLDER(constfold_sub)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (B.is_constant()) {
        if (is_zero(B)) {
            // R = A - 0   =>   R = A
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+1));
            return 1;
        }
    }
    if (A.is_constant() && B.is_constant()) {
        if (A.typespec().is_float() && B.typespec().is_float()) {
            float result = *(float *)A.data() - *(float *)B.data();
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        } else if (A.typespec().is_triple() && B.typespec().is_triple()) {
            Vec3 result = *(Vec3 *)A.data() - *(Vec3 *)B.data();
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        }
    }
    return 0;
}



DECLFOLDER(constfold_mul)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (A.is_constant()) {
        if (is_one(A)) {
            // R = 1 * B  =>   R = B
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+2));
            return 1;
        }
        if (is_zero(A)) {
            // R = 0 * B  =>   R = 0
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+1));
            return 1;
        }
    }
    if (B.is_constant()) {
        if (is_one(B)) {
            // R = A * 1   =>   R = A
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+1));
            return 1;
        }
        if (is_zero(B)) {
            // R = A * 0   =>   R = 0
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+2));
            return 1;
        }
    }
    if (A.is_constant() && B.is_constant()) {
        if (A.typespec().is_float() && B.typespec().is_float()) {
            float result = (*(float *)A.data()) * (*(float *)B.data());
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        } else if (A.typespec().is_triple() && B.typespec().is_triple()) {
            Vec3 result = (*(Vec3 *)A.data()) * (*(Vec3 *)B.data());
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        }
    }
    return 0;
}



DECLFOLDER(constfold_div)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (B.is_constant()) {
        if (is_one(B) || is_zero(B)) {
            // R = A / 1   =>   R = A
            // R = A / 0   =>   R = A      because of OSL div by zero rule
            turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+1));
            return 1;
        }
    }
    if (A.is_constant() && B.is_constant()) {
        if (A.typespec().is_float() && B.typespec().is_float()) {
            float result = *(float *)A.data() / *(float *)B.data();
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        } else if (A.typespec().is_triple() && B.typespec().is_triple()) {
            Vec3 result = *(Vec3 *)A.data() / *(Vec3 *)B.data();
            int cind = add_constant (inst, all_consts,
                                     next_newconst, A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        }
    }
    return 0;
}



DECLFOLDER(constfold_neg)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    if (A.is_constant()) {
        if (A.typespec().is_float()) {
            float result =  - *(float *)A.data();
            int cind = add_constant (inst, all_consts, next_newconst,
                                     A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        } else if (A.typespec().is_triple()) {
            Vec3 result = - *(Vec3 *)A.data();
            int cind = add_constant (inst, all_consts, next_newconst,
                                     A.typespec(), &result, true);
            turn_into_assign (op, inst.args(), cind);
            return 1;
        }
    }
    return 0;
}



DECLFOLDER(constfold_eq)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (A.is_constant() && B.is_constant()) {
        bool val = false;
        if (equivalent (A.typespec(), B.typespec())) {
            val = equal_consts (A, B);
        } else if (A.typespec().is_float() && B.typespec().is_int()) {
            val = (*(float *)A.data() == *(int *)B.data());
        } else if (A.typespec().is_int() && B.typespec().is_float()) {
            val = (*(int *)A.data() == *(float *)B.data());
        } else {
            return 0;  // unhandled cases
        }
        // Turn the 'eq R A B' into 'assign R X' where X is 0 or 1.
        static const int int_zero = 0, int_one = 1;
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_neq)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (A.is_constant() && B.is_constant()) {
        bool val = false;
        if (equivalent (A.typespec(), B.typespec())) {
            val = ! equal_consts (A, B);
        } else if (A.typespec().is_float() && B.typespec().is_int()) {
            val = (*(float *)A.data() != *(int *)B.data());
        } else if (A.typespec().is_int() && B.typespec().is_float()) {
            val = (*(int *)A.data() != *(float *)B.data());
        } else {
            return 0;  // unhandled case
        }
        // Turn the 'neq R A B' into 'assign R X' where X is 0 or 1.
        static const int int_zero = 0, int_one = 1;
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_lt)
{
    static const int int_zero = 0, int_one = 1;
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    const TypeSpec &ta (A.typespec()); 
    const TypeSpec &tb (B.typespec()); 
    if (A.is_constant() && B.is_constant()) {
        // Turn the 'leq R A B' into 'assign R X' where X is 0 or 1.
        bool val = false;
        if (ta.is_float() && tb.is_float()) {
            val = (*(float *)A.data() < *(float *)B.data());
        } else if (ta.is_float() && tb.is_int()) {
            val = (*(float *)A.data() < *(int *)B.data());
        } else if (ta.is_int() && tb.is_float()) {
            val = (*(int *)A.data() < *(float *)B.data());
        } else if (ta.is_int() && tb.is_int()) {
            val = (*(int *)A.data() < *(int *)B.data());
        } else {
            return 0;  // unhandled case
        }
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_le)
{
    static const int int_zero = 0, int_one = 1;
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    const TypeSpec &ta (A.typespec()); 
    const TypeSpec &tb (B.typespec()); 
    if (A.is_constant() && B.is_constant()) {
        // Turn the 'leq R A B' into 'assign R X' where X is 0 or 1.
        bool val = false;
        if (ta.is_float() && tb.is_float()) {
            val = (*(float *)A.data() <= *(float *)B.data());
        } else if (ta.is_float() && tb.is_int()) {
            val = (*(float *)A.data() <= *(int *)B.data());
        } else if (ta.is_int() && tb.is_float()) {
            val = (*(int *)A.data() <= *(float *)B.data());
        } else if (ta.is_int() && tb.is_int()) {
            val = (*(int *)A.data() <= *(int *)B.data());
        } else {
            return 0;  // unhandled case
        }
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_gt)
{
    static const int int_zero = 0, int_one = 1;
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    const TypeSpec &ta (A.typespec()); 
    const TypeSpec &tb (B.typespec()); 
    if (A.is_constant() && B.is_constant()) {
        // Turn the 'leq R A B' into 'assign R X' where X is 0 or 1.
        bool val = false;
        if (ta.is_float() && tb.is_float()) {
            val = (*(float *)A.data() > *(float *)B.data());
        } else if (ta.is_float() && tb.is_int()) {
            val = (*(float *)A.data() > *(int *)B.data());
        } else if (ta.is_int() && tb.is_float()) {
            val = (*(int *)A.data() > *(float *)B.data());
        } else if (ta.is_int() && tb.is_int()) {
            val = (*(int *)A.data() > *(int *)B.data());
        } else {
            return 0;  // unhandled case
        }
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_ge)
{
    static const int int_zero = 0, int_one = 1;
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    const TypeSpec &ta (A.typespec()); 
    const TypeSpec &tb (B.typespec()); 
    if (A.is_constant() && B.is_constant()) {
        // Turn the 'leq R A B' into 'assign R X' where X is 0 or 1.
        bool val = false;
        if (ta.is_float() && tb.is_float()) {
            val = (*(float *)A.data() >= *(float *)B.data());
        } else if (ta.is_float() && tb.is_int()) {
            val = (*(float *)A.data() >= *(int *)B.data());
        } else if (ta.is_int() && tb.is_float()) {
            val = (*(int *)A.data() >= *(float *)B.data());
        } else if (ta.is_int() && tb.is_int()) {
            val = (*(int *)A.data() >= *(int *)B.data());
        } else {
            return 0;  // unhandled case
        }
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_or)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (A.is_constant() && B.is_constant()) {
        DASSERT (A.typespec().is_int() && B.typespec().is_int());
        bool val = *(int *)A.data() || *(int *)B.data();
        // Turn the 'or R A B' into 'assign R X' where X is 0 or 1.
        static const int int_zero = 0, int_one = 1;
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_and)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &B (*inst.argsymbol(op.firstarg()+2));
    if (A.is_constant() && B.is_constant()) {
        DASSERT (A.typespec().is_int() && B.typespec().is_int());
        bool val = *(int *)A.data() && *(int *)B.data();
        // Turn the 'or R A B' into 'assign R X' where X is 0 or 1.
        static const int int_zero = 0, int_one = 1;
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 val ? &int_one : &int_zero);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_if)
{
    Opcode &op (inst.ops()[opnum]);
    Symbol &C (*inst.argsymbol(op.firstarg()+0));
    if (C.is_constant()) {
        int result = -1;   // -1 == we don't know
        if (C.typespec().is_int())
            result = (((int *)C.data())[0] != 0);
        else if (C.typespec().is_float())
            result = (((float *)C.data())[0] != 0.0f);
        else if (C.typespec().is_triple())
            result = (((Vec3 *)C.data())[0] != Vec3(0,0,0));
        else if (C.typespec().is_string()) {
            ustring s = ((ustring *)C.data())[0];
            result = (s.length() != 0);
        }
        int changed = 0;
        if (result > 0) {
            for (int i = op.jump(0);  i < op.jump(1);  ++i, ++changed)
                inst.ops()[i].reset (ustring("nop"), OP_nop, 0);
            op.reset (ustring("nop"), OP_nop, 0);
            return changed+1;
        } else if (result == 0) {
            for (int i = opnum+1;  i < op.jump(0);  ++i, ++changed)
                inst.ops()[i].reset (ustring("nop"), OP_nop, 0);
            op.reset (ustring("nop"), OP_nop, 0);
            return changed+1;
        }
    }
    return 0;
}



DECLFOLDER(constfold_aref)
{
    // Array reference -- crops up more than you think in production shaders!
    // Try to turn R=A[I] into R=C if A and I are const.
    Opcode &op (inst.ops()[opnum]);
    Symbol &R (*inst.argsymbol(op.firstarg()+0));
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &Index (*inst.argsymbol(op.firstarg()+2));
    DASSERT (A.typespec().is_array() && Index.typespec().is_int());
    if (A.is_constant() && Index.is_constant()) {
        TypeSpec elemtype = A.typespec().elementtype();
        ASSERT (elemtype.is_float() || elemtype.is_triple() || elemtype.is_int());
        ASSERT (equivalent(elemtype, R.typespec()));
        int index = *(int *)Index.data();
        DASSERT (index < A.typespec().arraylength());
        int cind = add_constant (inst, all_consts, next_newconst, elemtype,
                                 (char *)A.data() + index*elemtype.simpletype().size(), true);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_compref)
{
    // Component reference
    // Try to turn R=A[I] into R=C if A and I are const.
    Opcode &op (inst.ops()[opnum]);
    Symbol &A (*inst.argsymbol(op.firstarg()+1));
    Symbol &Index (*inst.argsymbol(op.firstarg()+2));
    if (A.is_constant() && Index.is_constant()) {
        ASSERT (A.typespec().is_triple() && Index.typespec().is_int());
        int index = *(int *)Index.data();
        int cind = add_constant (inst, all_consts, next_newconst,
                                 TypeDesc::TypeFloat, (float *)A.data() + index);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_strlen)
{
    // Try to turn R=strlen(s) into R=C
    Opcode &op (inst.ops()[opnum]);
    Symbol &S (*inst.argsymbol(op.firstarg()+1));
    if (S.is_constant()) {
        ASSERT (S.typespec().is_string());
        int result = (int) (*(ustring *)S.data()).length();
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 &result, true /* make a copy */);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_endswith)
{
    // Try to turn R=endswith(s,e) into R=C
    Opcode &op (inst.ops()[opnum]);
    Symbol &S (*inst.argsymbol(op.firstarg()+1));
    Symbol &E (*inst.argsymbol(op.firstarg()+2));
    if (S.is_constant() && E.is_constant()) {
        ASSERT (S.typespec().is_string() && E.typespec().is_string());
        ustring s = *(ustring *)S.data();
        ustring e = *(ustring *)E.data();
        size_t elen = e.length(), slen = s.length();
        int result = 0;
        if (elen <= slen)
            result = (strncmp (s.c_str()+slen-elen, e.c_str(), elen) == 0);
        int cind = add_constant (inst, all_consts,
                                 next_newconst, TypeDesc::TypeInt,
                                 &result, true /* make a copy */);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_concat)
{
    // Try to turn R=concat(s,...) into R=C
    Opcode &op (inst.ops()[opnum]);
    ustring result;
    for (int i = 1;  i < op.nargs();  ++i) {
        Symbol &S (*inst.argsymbol(op.firstarg()+i));
        if (! S.is_constant())
            return 0;  // something non-constant
        ustring old = result;
        ustring s = *(ustring *)S.data();
        result = ustring::format ("%s%s", old.c_str() ? old.c_str() : "",
                                  s.c_str() ? s.c_str() : "");
    }
    // If we made it this far, all args were constants, and the
    // concatenation is in result.
    int cind = add_constant (inst, all_consts,
                             next_newconst, TypeDesc::TypeString,
                             &result, true /* make a copy */);
    turn_into_assign (op, inst.args(), cind);
    return 1;
}



inline float clamp (float x, float minv, float maxv)
{
    if (x < minv) return minv;
    else if (x > maxv) return maxv;
    else return x;
}



DECLFOLDER(constfold_clamp)
{
    // Try to turn R=clamp(x,min,max) into R=C
    Opcode &op (inst.ops()[opnum]);
    Symbol &X (*inst.argsymbol(op.firstarg()+1));
    Symbol &Min (*inst.argsymbol(op.firstarg()+2));
    Symbol &Max (*inst.argsymbol(op.firstarg()+3));
    if (X.is_constant() && Min.is_constant() && Max.is_constant()) {
        DASSERT (equivalent(X.typespec(), Min.typespec()) &&
                 equivalent(X.typespec(), Max.typespec()));
        DASSERT (X.typespec().is_float() || X.typespec().is_triple());
        const float *x = (const float *) X.data();
        const float *min = (const float *) Min.data();
        const float *max = (const float *) Max.data();
        float result[3];
        result[0] = clamp (x[0], min[0], max[0]);
        if (X.typespec().is_triple()) {
            result[1] = clamp (x[1], min[1], max[1]);
            result[2] = clamp (x[2], min[2], max[2]);
        }
        int cind = add_constant (inst, all_consts,
                                 next_newconst, X.typespec(),
                                 &result, true /* make a copy */);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_sqrt)
{
    // Try to turn R=sqrt(x) into R=C
    Opcode &op (inst.ops()[opnum]);
    Symbol &X (*inst.argsymbol(op.firstarg()+1));
    if (X.is_constant() &&
          (X.typespec().is_float() || X.typespec().is_triple())) {
        const float *x = (const float *) X.data();
        float result[3];
        result[0] = sqrtf (std::max (0.0f, x[0]));
        if (X.typespec().is_triple()) {
            result[1] = sqrtf (std::max (0.0f, x[1]));
            result[2] = sqrtf (std::max (0.0f, x[2]));
        }
        int cind = add_constant (inst, all_consts,
                                 next_newconst, X.typespec(),
                                 &result, true /* make a copy */);
        turn_into_assign (op, inst.args(), cind);
        return 1;
    }
    return 0;
}



DECLFOLDER(constfold_matrix)
{
    // Try to turn R=matrix(from,to) into R=1 if it's an identity transform
    Opcode &op (inst.ops()[opnum]);
    if (op.nargs() == 3) {
        Symbol &From (*inst.argsymbol(op.firstarg()+1));
        Symbol &To (*inst.argsymbol(op.firstarg()+2));
        if (From.is_constant() && From.typespec().is_string() &&
            To.is_constant() && To.typespec().is_string()) {
            ustring from = *(ustring *)From.data();
            ustring to = *(ustring *)To.data();
            ustring commonsyn = inst.shadingsys().commonspace_synonym();
            if (from == to || (from == Strings::common && to == commonsyn) ||
                              (from == commonsyn && to == Strings::common)) {
                static Matrix44 ident (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
                int cind = add_constant (inst, all_consts, next_newconst,
                                         TypeDesc::TypeMatrix, &ident);
                turn_into_assign (op, inst.args(), cind);
                return 1;
            }
        }
    }
    return 0;
}



DECLFOLDER(constfold_transform)
{
    // Try to turn R=transform(M,P) into R=P if it's an identity transform
    Opcode &op (inst.ops()[opnum]);
    Symbol &M (*inst.argsymbol(op.firstarg()+1));
    Symbol &P (*inst.argsymbol(op.firstarg()+2));
    if (op.nargs() == 3 && M.typespec().is_matrix() &&
          M.is_constant() && is_one(M)) {
        ASSERT (P.typespec().is_triple());
        turn_into_assign (op, inst.args(), inst.arg(op.firstarg()+2));
        return 1;
    }
    return 0;
}




DECLFOLDER(constfold_useparam)
{
    // Just eliminate useparam (from shaders compiled with old oslc)
    Opcode &op (inst.ops()[opnum]);
    op.reset (ustring("nop"), OP_nop, 0);
    return 1;
}



static std::map<ustring,OpFolder> folder_table;


void
initialize_folder_table ()
{
    static spin_mutex folder_table_mutex;
    static bool folder_table_initialized = false;
    spin_lock lock (folder_table_mutex);
    if (folder_table_initialized)
        return;   // already initialized
#define INIT2(name,folder) folder_table[ustring(#name)] = folder
#define INIT(name) folder_table[ustring(#name)] = constfold_##name;

    INIT (add);    INIT (sub);
    INIT (mul);    INIT (div);
    INIT (neg);
    INIT (eq);     INIT (neq);
    INIT (le);     INIT (ge);
    INIT (lt);     INIT (gt);
    INIT (or);     INIT (and);
    INIT (if);
    INIT (compref);
    INIT (aref);
    INIT (strlen);
    INIT (endswith);
    INIT (concat);
    INIT (clamp);
    INIT (sqrt);
    INIT (matrix);
    INIT2 (transform, constfold_transform);
    INIT2 (transformv, constfold_transform);
    INIT2 (transformn, constfold_transform);
    INIT (useparam);
#undef INIT
#undef INIT2

    folder_table_initialized = true;
}



// Identify basic blocks by assigning a basic block ID for each
// instruction.  Within any basic bock, there are no jumps in or out.
// Also note which instructions are inside conditional states.
void
find_basic_blocks (OpcodeVec &code, SymbolVec &symbols,
                   std::vector<int> &bblockids,
                   std::vector<bool> &in_conditional, int maincodebegin)
{
    in_conditional.clear ();
    in_conditional.resize (code.size(), false);
    for (int i = 0;  i < (int)code.size();  ++i) {
        if (code[i].jump(0) >= 0)
            std::fill (in_conditional.begin()+i,
                       in_conditional.begin()+code[i].farthest_jump(), true);
    }

    // Start by setting all basic block IDs to 0
    bblockids.clear ();
    bblockids.resize (code.size(), 0);
    int bbid = 1;  // next basic block ID to use

    // First, keep track of all the spots where blocks begin
    std::vector<bool> block_begin (code.size(), false);

    // Init ops start basic blocks
    BOOST_FOREACH (const Symbol &s, symbols) {
        if (s.symtype() == SymTypeParam || s.symtype() == SymTypeOutputParam &&
                s.initbegin() >= 0)
            block_begin[s.initbegin()] = true;
    }

    // Main code starts a basic block
    block_begin[maincodebegin] = true;

    // Anyplace that's the target of a jump instruction starts a basic block
    for (int i = 0;  i < (int)code.size();  ++i) {
        for (int j = 0;  j < (int)Opcode::max_jumps;  ++j) {
            if (code[i].jump(j) >= 0)
                block_begin[code[i].jump(j)] = true;
            else
                break;
        }
    }

    // Now color the blocks with unique identifiers
    for (int i = 0;  i < (int)code.size();  ++i) {
        if (block_begin[i])
            ++bbid;
        bblockids[i] = bbid;
    }
}



void
ShadingSystemImpl::optimize_instance (ShaderGroup &group, int layer,
                                      ShaderInstance &inst)
{
    if (debug()) {
        std::cerr << "Optimizing layer " << layer << " " << inst.layername()
                  << ' ' << inst.shadername() << "\n";
        std::cerr << "Before optimizing instance, I get: \n" << inst.print() 
                  << "\n--------------------------------\n\n";
    }

    initialize_folder_table ();

    size_t old_nsyms = inst.symbols().size();
    size_t old_nops = inst.ops().size();

    // Start by making a list of the indices of all constants.
    std::vector<int> all_consts;
    for (int i = 0;  i < (int)inst.symbols().size();  ++i)
        if (inst.symbol(i)->symtype() == SymTypeConst)
            all_consts.push_back (i);

    typedef std::map<int,int> IntMap;
    IntMap symbol_aliases;   // Track symbol aliases
    int next_newconst = 0;   // Index of next new constant

    // Turn all geom-locked parameters into constants.  While we're at it,
    // build up the list of all constants.
    if (optimize() >= 2) {
        for (int i = inst.firstparam();  i <= inst.lastparam();  ++i) {
            Symbol *s (inst.symbol(i));
            if (s->symtype() == SymTypeParam && s->lockgeom() &&
                s->valuesource() != Symbol::ConnectedVal &&
                !(s->valuesource() == Symbol::DefaultVal && s->initbegin() < s->initend()) &&
                !s->typespec().is_structure() && !s->typespec().is_closure()) {
                // Make a new const using the param's instance values
                inst.make_symbol_room (1);
                s = inst.symbol(i);  // In case make_symbol_room changed ptrs
                int cind = add_constant (inst, all_consts,
                                         next_newconst, s->typespec(), s->data());
                // Alias this symbol to the new const
                symbol_aliases[i] = cind;
            }
        }
    }

    // Try to fold constants.  We take several passes, until we get to
    // the point that not much is improving.  It rarely goes beyond 3-4
    // passes, but we have a hard cutoff at 10 just to be sure we don't
    // ever get into an infinite loop from an unforseen cycle.  where we
    // end up inadvertently transforming A => B => A => etc.
    SymbolPtrVec allsymptrs;
    int totalchanged = 0;
    for (int pass = 0;  pass < 10;  ++pass) {

        // Track basic blocks and conditional states
        std::vector<bool> in_conditional (inst.ops().size(), false);
        std::vector<int> bblockids;
        find_basic_blocks (inst.ops(), inst.symbols(),
                           bblockids, in_conditional, inst.m_maincodebegin);

        // Constant aliases valid for just this basic block
        std::vector<int> block_aliases;
        block_aliases.resize (inst.symbols().size(), -1);

        int changed = 0;
        int lastblock = -1;
        for (int opnum = 0;  opnum < (int)inst.ops().size();  ++opnum) {
            Opcode &op (inst.ops()[opnum]);
            ASSERT (&op == &(inst.ops()[opnum]));
            // Find the farthest this instruction jumps to (-1 for ops
            // that don't jump) so we can mark conditional regions.
            int jumpend = op.farthest_jump();
            for (int i = (int)opnum+1;  i < jumpend;  ++i)
                in_conditional[i] = true;

            // If we've just moved to a new basic block, clear the aliases
            if (lastblock != bblockids[opnum]) {
                block_aliases.clear ();
                block_aliases.resize (inst.symbols().size(), -1);
                lastblock = bblockids[opnum];
            }

            // De-alias the args to the op and figure out if there are
            // any constants involved.
            bool any_const_args = false;
            for (int i = 0;  i < op.nargs();  ++i) {
                int argindex = op.firstarg() + i;
                int argsymindex = inst.arg(argindex);
                if (op.argwrite(i)) {
                    // Written arg -> no longer aliases anything
                    block_aliases[argsymindex] = -1;
                    continue;    // Don't alias args that are written
                }
                do {
                    if (block_aliases[argsymindex] >= 0 && ! op.argwrite(i)) {
                        // block-specific alias for the sym
                        inst.args()[argindex] = block_aliases[argsymindex];
                        continue;
                    }
                    IntMap::const_iterator found;
                    found = symbol_aliases.find (argsymindex);
                    if (found != symbol_aliases.end()) {
                        // permanent alias for the sym
                        inst.args()[argindex] = found->second;
                        continue;
                    }
                } while (0);
                any_const_args |= inst.argsymbol(argindex)->is_constant();
            }

            // We aren't currently doing anything to optimize ops that have
            // no const args, so don't bother looking further in that case.
            if (! any_const_args)
                continue;

            // Make sure there's room for at least one more symbol, so that
            // we can add a const if we need to, without worrying about the
            // addresses of symbols changing when we add a new one below.
            inst.make_symbol_room (1);

            // For various ops that we know how to effectively
            // constant-fold, dispatch to the appropriate routine.
            //
            // FIXME -- eventually replace the following cascacing 'if'
            // with a hash lookup and single function call.
            if (optimize() >= 2) {
                std::map<ustring,OpFolder>::const_iterator found;
                found = folder_table.find (op.opname());
                if (found != folder_table.end())
                    changed = (*found->second) (inst, opnum,
                                                all_consts, next_newconst);
            }

            // Now we handle assignments.
            //
            // N.B. This is a regular "if", not an "else if", because we
            // definitely want to catch any 'assign' statements that
            // were put in by the constant folding routines above.
            if (optimize() >= 2 && op.implementation() == OP_assign) {
                Symbol *R (inst.argsymbol(op.firstarg()+0));
                Symbol *A (inst.argsymbol(op.firstarg()+1));
                bool R_local_or_tmp = (R->symtype() == SymTypeLocal ||
                                       R->symtype() == SymTypeTemp);

                // Odd but common case: two instructions in a row assign
                // to the same variable.  Get rid of the first.
                for (int f = opnum+1;  f < (int)inst.ops().size() && bblockids[f] == bblockids[opnum];  ++f) {
                    Opcode &opf (inst.ops()[f]);
                    if (opf.implementation() == OP_nop)
                        continue;
                    if (opf.implementation() == OP_assign) {
                        if (inst.argsymbol(opf.firstarg()) == R) {
                            // Both assigning to the same variable!  Kill one.
                            op.reset (ustring("nop"), OP_nop, 0);
                            ++changed;
                        }
                    }
                    break;
                }
                if (op.implementation() == OP_nop)
                    continue;

                if (block_aliases[inst.arg(op.firstarg())] == inst.arg(op.firstarg()+1) ||
                    block_aliases[inst.arg(op.firstarg()+1)] == inst.arg(op.firstarg())) {
                    // We're re-assigning something already aliased, skip it
                    op.reset (ustring("nop"), OP_nop, 0);
                    ++changed;
                    continue;
                }
                if (A->is_constant() && A->typespec().is_int() &&
                    ! R->typespec().is_closure() && R->typespec().is_float()) {
                    float result = *(int *)A->data();
                    int cind = add_constant (inst, all_consts, next_newconst,
                                             R->typespec(),
                                             &result, true /* make a copy */);
                    turn_into_assign (op, inst.args(), cind);
                    ++changed;
                }

                if (A->is_constant() &&
                        equivalent(R->typespec(), A->typespec())) {
                    block_aliases[inst.arg(op.firstarg())] =
                        inst.arg(op.firstarg()+1);
//                  std::cerr << opnum << " aliasing " << R->mangled() " to "
//                        << inst.argsymbol(op.firstarg()+1)->mangled() << "\n";
                }

                if (A->is_constant() && R->typespec() == A->typespec() &&
                    R_local_or_tmp &&
                    R->firstwrite() == opnum && R->lastwrite() == opnum) {
                    // This local or temp is written only once in the
                    // whole shader -- on this statement -- and it's
                    // assigned a constant.  So just alias it to the
                    // constant.
                    int cind = inst.args()[op.firstarg()+1];
                    symbol_aliases[inst.args()[op.firstarg()]] = cind;
                    op.reset (ustring("nop"), OP_nop, 0);
                    ++changed;
                    continue;
                }
                else if (A->is_constant() && R->typespec() == A->typespec() &&
                         R->symtype() == SymTypeOutputParam &&
                         R->firstwrite() == opnum && R->lastwrite() == opnum &&
                         !in_conditional[opnum]) {
                    // This output param is written only once in the
                    // whole shader -- on this statement -- and it's
                    // assigned a constant, and the assignment is
                    // unconditional.  So just alias it to the constant from
                    // here on out.
                    int cind = inst.args()[op.firstarg()+1];
                    symbol_aliases[inst.args()[op.firstarg()]] = cind;
                }
                if (R == A) {
                    // Just an assignment to itself -- turn into NOP!
                    op.reset (ustring("nop"), OP_nop, 0);
                    ++changed;
                } else if (R_local_or_tmp && R->lastread() < opnum) {
                    // Don't bother assigning if we never read it again
                    op.reset (ustring("nop"), OP_nop, 0);
                    ++changed;
                }
            }
        }

        totalchanged += changed;
        // info ("Pass %d, changed %d\n", pass, changed);

        // Now that we've rewritten the code, we need to re-track the
        // variable lifetimes.
        allsymptrs.clear ();
        allsymptrs.reserve (inst.symbols().size());
        BOOST_FOREACH (Symbol &s, inst.symbols())
            allsymptrs.push_back (&s);
        {
            SymbolPtrVec oparg_ptrs;
            BOOST_FOREACH (int a, inst.args())
                oparg_ptrs.push_back (inst.symbol (a));
            OSLCompilerImpl::track_variable_lifetimes (inst.ops(), oparg_ptrs, allsymptrs);
        }

        // If only a couple things changed, we know that we almost never get
        // more benefit from another pass, so avoid the expense.
        if (changed < 1)
            break;
    }

    add_useparam (inst.ops(), inst.args(), allsymptrs, inst.m_maincodebegin);
    {
        SymbolPtrVec oparg_ptrs;
        BOOST_FOREACH (int a, inst.args())
            oparg_ptrs.push_back (inst.symbol (a));
        OSLCompilerImpl::track_variable_lifetimes (inst.ops(), oparg_ptrs, allsymptrs);
    }
    if (optimize() >= 1) {
        OSLCompilerImpl::coalesce_temporaries (allsymptrs);
        // coalesce_temporaries may have aliased temps, now we need to
        // make sure all symbol refs are dealiased.
        BOOST_FOREACH (int &arg, inst.m_instargs) {
            Symbol *s = &(inst.symbols()[arg]);
            s = s->dealias ();
            arg = s - &(inst.symbols()[0]);
        }
    }

    // Get rid of nop instructions and unused symbols.
    if (optimize() >= 1) {
        collapse (group, layer, inst);
        info ("Optimized %s: New syms %llu/%llu, ops %llu/%llu",
              inst.shadername().c_str(),
              inst.symbols().size(), old_nsyms,
              inst.ops().size(), old_nops);
    } else {
        inst.m_maincodeend = (int)inst.ops().size();
        info ("Processed %s", inst.shadername().c_str());
    }

    if (debug())
        std::cerr << "After optimizing: \n" << inst.print() 
                  << "\n--------------------------------\n\n";
}



void
ShadingSystemImpl::collapse (ShaderGroup &group, int layer,
                             ShaderInstance &inst)
{
    //
    // Make a new symbol table that removes all the unused symbols.
    //

#if 0  // FIXME -- come back to this
    // Mark our params that feed to later layers
    for (int lay = layer+1;  lay < group.nlayers();  ++lay) {
        BOOST_FOREACH (Connection &c, group[lay]->m_connections)
            if (c.srclayer == layer)
                inst.symbol(c.src.param)->connected_down (true);
    }
#endif

    SymbolVec new_symbols;          // buffer for new symbol table
    std::vector<int> symbol_remap;  // mapping of old sym index to new
    int total_syms = 0;             // number of new symbols we'll need

    // First, just count how many we need and set up the mapping
    BOOST_FOREACH (const Symbol &s, inst.symbols()) {
        symbol_remap.push_back (total_syms);
        if (s.everused() ||
            (s.symtype() == SymTypeParam /*&& s.connected_down()*/) ||
              s.symtype() == SymTypeOutputParam)
            ++total_syms;
    }

    // Now make a new table of the right (new) size, and copy the used syms
    new_symbols.reserve (total_syms);
    BOOST_FOREACH (const Symbol &s, inst.symbols()) {
        if (s.everused() ||
            (s.symtype() == SymTypeParam /*&& s.connected_down()*/) ||
              s.symtype() == SymTypeOutputParam)
            new_symbols.push_back (s);
    }

    // Remap all the function arguments to the new indices
    BOOST_FOREACH (int &arg, inst.m_instargs)
        arg = symbol_remap[arg];

    // Fix our connections from upstream shaders
    BOOST_FOREACH (Connection &c, inst.m_connections)
        c.dst.param = symbol_remap[c.dst.param];

    // Fix downstream connections that reference us
    for (int lay = layer+1;  lay < group.nlayers();  ++lay) {
        BOOST_FOREACH (Connection &c, group[lay]->m_connections)
            if (c.srclayer == layer)
                c.src.param = symbol_remap[c.src.param];
    }

    // Miscellaneous cleanup of other things that used symbol indices
    if (inst.m_Psym >= 0)
        inst.m_Psym = symbol_remap[inst.m_Psym];
    if (inst.m_Nsym >= 0)
        inst.m_Nsym = symbol_remap[inst.m_Nsym];
    if (inst.m_lastparam >= 0) {
        inst.m_firstparam = symbol_remap[inst.m_firstparam];
        inst.m_lastparam = symbol_remap[inst.m_lastparam];
    }

    // Swap the new symbol list for the old.
    std::swap (inst.m_instsymbols, new_symbols);


    //
    // Make new code that removes all the nops
    //
    OpcodeVec new_ops;              // buffer for new code
    std::vector<int> op_remap;      // mapping of old opcode indices to new
    int total_ops = 0;              // number of new ops we'll need

    // First, just count how many we need and set up the mapping
    BOOST_FOREACH (const Opcode &op, inst.ops()) {
        op_remap.push_back (total_ops);
        if (op.implementation() != OP_nop)
            ++total_ops;
    }

    // Now make a new table of the right (new) size, copy the used ops, and
    // reset the jump addresses.
    new_ops.reserve (total_ops);
    BOOST_FOREACH (const Opcode &op, inst.ops()) {
        if (op.implementation() != OP_nop) {
            new_ops.push_back (op);
            Opcode &newop (new_ops.back());
            for (int i = 0;  i < (int)Opcode::max_jumps;  ++i)
                if (newop.jump(i) >= 0)
                    newop.jump(i) = op_remap[newop.jump(i)];
        }
    }

    // Miscellaneous cleanup of other things that used instruction addresses
    inst.m_maincodebegin = op_remap[inst.m_maincodebegin];
    inst.m_maincodeend = (int)new_ops.size();

    // Swap the new code for the old.
    std::swap (inst.m_instops, new_ops);
}



void
ShadingSystemImpl::optimize_group (ShadingAttribState &attribstate, 
                                   ShaderGroup &group)
{
    Timer timer;
    lock_guard lock (group.m_mutex);
    if (group.optimized()) {
        spin_lock (m_stat_mutex);
        m_stat_optimization_time += timer();
        return;
    }
    int nlayers = (int) group.nlayers ();
    for (int layer = 0;  layer < nlayers;  ++layer) {
        ShaderInstance *inst = group[layer];
        if (inst) {
            optimize_instance (group, layer, *inst);
            // ASSERT (!inst->m_heap_size_calculated);
            inst->m_heap_size_calculated = false;
        }
    }
    attribstate.changed_shaders ();
    group.m_optimized = true;
    spin_lock (m_stat_mutex);
    m_stat_optimization_time += timer();
}



}; // namespace pvt
}; // namespace OSL

#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
#endif