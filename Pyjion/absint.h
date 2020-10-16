/*
* The MIT License (MIT)
*
* Copyright (c) Microsoft Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
*/

#ifndef ABSINT_H
#define ABSINT_H

#include <Python.h>
#include <vector>
#include <unordered_map>

#include "absvalue.h"
#include "cowvector.h"
#include "ipycomp.h"
#include "exceptionhandling.h"
#include "stack.h"


using namespace std;

struct AbstractLocalInfo;

// Tracks block information for analyzing loops, exception blocks, and break opcodes.
struct AbsIntBlockInfo {
    size_t BlockStart, BlockEnd;
    __unused bool IsLoop;

    AbsIntBlockInfo(size_t blockStart, size_t blockEnd, bool isLoop) {
        BlockStart = blockStart;
        BlockEnd = blockEnd;
        IsLoop = isLoop;
    }
};

// The abstract interpreter implementation.  The abstract interpreter performs
// static analysis of the Python byte code to determine what types are known.
// Ultimately this information will feedback into code generation allowing
// more efficient code to be produced.
//
// The abstract interpreter ultimately produces a set of states for each opcode
// before it has been executed.  It also produces an abstract value for the type
// that the function returns.
//
// The abstract interpreter walks the byte code updating the stack of the stack
// and locals based upon the opcode being performed and the existing state of the
// stack.  When it encounters a branch it will merge the current state in with the
// state for where we're branching to.  If the merge results in a new starting state
// that we haven't analyzed it will then queue the target opcode as the next starting
// point to be analyzed.
//
// If the branch is unconditional, or definitively taken based upon analysis, then
// we'll go onto the next starting opcode to be analyzed.
//
// Once we've processed all of the blocks of code in this manner the analysis
// is complete.

struct AbstractValueKindHash {
    std::size_t operator()(AbstractValueKind e) const {
        return static_cast<std::size_t>(e);
    }
};


struct BlockInfo {
    int EndOffset, Kind, ContinueOffset;
    EhFlags Flags;
    ExceptionHandler* CurrentHandler;  // the current exception handler
    __unused Local LoopVar; //, LoopOpt1, LoopOpt2;

    BlockInfo(int endOffset, int kind, ExceptionHandler* currentHandler, EhFlags flags = EHF_None, int continueOffset = 0) {
        EndOffset = endOffset;
        Kind = kind;
        Flags = flags;
        CurrentHandler = currentHandler;
        ContinueOffset = continueOffset;
    }
};


// Tracks the state of a local variable at each location in the function.
// Each local has a known type associated with it as well as whether or not
// the value is potentially undefined.  When a variable is definitely assigned
// IsMaybeUndefined is false.
//
// Initially all locals start out as being marked as IsMaybeUndefined and a special
// typeof Undefined.  The special type is really just for convenience to avoid
// having null types.  Merging with the undefined type will produce the other type.
// Assigning to a variable will cause the undefined marker to be removed, and the
// new type to be specified.
//
// When we merge locals if the undefined flag is specified from either side we will
// propagate it to the new state.  This could result in:
//
// State 1: Type != Undefined, IsMaybeUndefined = false
//      The value is definitely assigned and we have valid type information
//
// State 2: Type != Undefined, IsMaybeUndefined = true
//      The value is assigned in one code path, but not in another.
//
// State 3: Type == Undefined, IsMaybeUndefined = true
//      The value is definitely unassigned.
//
// State 4: Type == Undefined, IsMaybeUndefined = false
//      This should never happen as it means the Undefined
//      type has leaked out in an odd way
struct AbstractLocalInfo {
    AbstractLocalInfo() = default;

    AbstractValueWithSources ValueInfo;
    bool IsMaybeUndefined;

    AbstractLocalInfo(AbstractValueWithSources valueInfo, bool isUndefined = false) : ValueInfo(valueInfo) {
        IsMaybeUndefined = true;
        assert(valueInfo.Value != nullptr);
        assert(!(valueInfo.Value == &Undefined && !isUndefined));
        IsMaybeUndefined = isUndefined;
    }

    AbstractLocalInfo merge_with(AbstractLocalInfo other) const {
        return {
            ValueInfo.merge_with(other.ValueInfo),
            IsMaybeUndefined || other.IsMaybeUndefined
            };
    }

    bool operator== (AbstractLocalInfo other) {
        return other.ValueInfo == ValueInfo &&
            other.IsMaybeUndefined == IsMaybeUndefined;
    }
    bool operator!= (AbstractLocalInfo other) {
        return other.ValueInfo != ValueInfo ||
            other.IsMaybeUndefined != IsMaybeUndefined;
    }
};

// Represents the state of the program at each opcode.  Captures the state of both
// the Python stack and the local variables.  We store the state for each opcode in
// AbstractInterpreter.m_startStates which represents the state before the indexed
// opcode has been executed.
//
// The stack is a unique vector for each interpreter state.  There's currently no
// attempts at sharing because most instructions will alter the value stack.
//
// The locals are shared between InterpreterState's using a shared_ptr because the
// values of locals won't change between most opcodes (via CowVector).  When updating
// a local we first check if the locals are currently shared, and if not simply update
// them in place.  If they are shared then we will issue a copy.
class InterpreterState {
public:
    vector<AbstractValueWithSources> m_stack;
    CowVector<AbstractLocalInfo> m_locals;

    InterpreterState() = default;

    explicit InterpreterState(int numLocals) {
        m_locals = CowVector<AbstractLocalInfo>(numLocals);
    }

    AbstractLocalInfo get_local(size_t index) {
        return m_locals[index];
    }

    size_t local_count() {
        return m_locals.size();
    }

    void replace_local(size_t index, AbstractLocalInfo value) {
        m_locals.replace(index, value);
    }

    AbstractValue* pop() {
        assert(!m_stack.empty());
        auto res = m_stack.back();
        res.escapes();
        m_stack.pop_back();
        return res.Value;
    }

    AbstractValueWithSources pop_no_escape() {
        assert(!m_stack.empty());
        auto res = m_stack.back();
        m_stack.pop_back();
        return res;
    }

    void push(AbstractValueWithSources& value) {
        m_stack.push_back(value);
    }

    void push(AbstractValue* value) {
        // TODO : Use emplace_back instead?
        m_stack.push_back(value);
    }

    size_t stack_size() const {
        return m_stack.size();
    }

    AbstractValueWithSources& operator[](const size_t index) {
        return m_stack[index];
    }
};


#ifdef _WIN32
class __declspec(dllexport) AbstractInterpreter {
#pragma warning (disable:4251)
#else
class AbstractInterpreter {
#endif
    // ** Results produced:
    // Tracks the interpreter state before each opcode
    unordered_map<size_t, InterpreterState> m_startStates;
    AbstractValue* m_returnValue;

    // ** Inputs:
    PyCodeObject* m_code;
    _Py_CODEUNIT *m_byteCode;
    size_t m_size;
    Local m_errorCheckLocal;

    // ** Data consumed during analysis:
    // Tracks the entry point for each POP_BLOCK opcode, so we can restore our
    // stack state back after the POP_BLOCK
    unordered_map<size_t, size_t> m_blockStarts;
    // Tracks the location where each BREAK_LOOP will break to, so we can merge
    // state with the current state to the breaked location.
    unordered_map<size_t, AbsIntBlockInfo> m_breakTo;
    unordered_map<size_t, AbstractSource*> m_opcodeSources;
    // all values produced during abstract interpretation, need to be freed
    vector<AbstractValue*> m_values;
    vector<AbstractSource*> m_sources;
    vector<Local> m_raiseAndFreeLocals;
    IPythonCompiler* m_comp;
    // m_blockStack is like Python's f_blockstack which lives on the frame object, except we only maintain
    // it at compile time.  Blocks are pushed onto the stack when we enter a loop, the start of a try block,
    // or into a finally or exception handler.  Blocks are popped as we leave those protected regions.
    // When we pop a block associated with a try body we transform it into the correct block for the handler
    vector<BlockInfo> m_blockStack;

    ExceptionHandlerManager m_exceptionHandler;
    // Labels that map from a Python byte code offset to an ilgen label.  This allows us to branch to any
    // byte code offset.
    unordered_map<int, Label> m_offsetLabels;
    // Tracks the depth of the Python stack
    size_t m_blockIds;
    // Tracks the current depth of the stack,  as well as if we have an object reference that needs to be freed.
    // True (STACK_KIND_OBJECT) if we have an object, false (STACK_KIND_VALUE) if we don't
    Stack m_stack;
    // Tracks the state of the stack when we perform a branch.  We copy the existing state to the map and
    // reload it when we begin processing at the stack.
    unordered_map<int, Stack> m_offsetStack;
    // Set of labels used for when we need to raise an error but have values on the stack
    // that need to be freed.  We have one set of labels which fall through to each other
    // before doing the raise:
    //      free2: <decref>/<pop>
    //      free1: <decref>/<pop>
    //      raise logic.
    //  This was so we don't need to have decref/frees spread all over the code
    vector<vector<Label>> m_raiseAndFree, m_reraiseAndFree;
    unordered_set<size_t> m_jumpsTo;
    Label m_retLabel;
    Local m_retValue;
    // Stores information for a stack allocated local used for sequence unpacking.  We need to allocate
    // one of these when we enter the method, and we use it if we don't have a sequence we can efficiently
    // unpack.
    unordered_map<int, Local> m_sequenceLocals;
    unordered_map<int, bool> m_assignmentState;
    unordered_map<int, unordered_map<AbstractValueKind, Local, AbstractValueKindHash>> m_optLocals;

#pragma warning (default:4251)

public:
    AbstractInterpreter(PyCodeObject *code, IPythonCompiler* compiler);
    ~AbstractInterpreter();

    JittedCode* compile();
    bool interpret();
    void dump();

    void set_local_type(int index, AbstractValueKind kind);
    // Returns information about the specified local variable at a specific
    // byte code index.
    AbstractLocalInfo get_local_info(size_t byteCodeIndex, size_t localIndex);

    // Returns information about the stack at the specific byte code index.
    vector<AbstractValueWithSources>& get_stack_info(size_t byteCodeIndex);

    // Returns true if the result of the opcode should be boxed, false if it
    // can be maintained on the stack.
    bool should_box(size_t opcodeIndex);

    bool can_skip_lasti_update(size_t opcodeIndex);

    AbstractValue* get_return_info();

    __unused bool has_info(size_t byteCodeIndex);

private:
    void compile_pop_block();
    void compile_pop_except_block();
    AbstractValue* to_abstract(PyObject* obj);
    AbstractValue* to_abstract(AbstractValueKind kind);
    bool merge_states(InterpreterState& newState, InterpreterState& mergeTo);
    bool update_start_state(InterpreterState& newState, size_t index);
    void init_starting_state();
    const char* opcode_name(int opcode);
    bool preprocess();
    void dump_sources(AbstractSource* sources);
    AbstractSource* new_source(AbstractSource* source) {
        m_sources.push_back(source);
        return source;
    }

    AbstractSource* add_local_source(size_t opcodeIndex, size_t localIndex);
    AbstractSource* add_const_source(size_t opcodeIndex, size_t constIndex);
    AbstractSource* add_intermediate_source(size_t opcodeIndex);

    void make_function(int oparg);
    bool can_skip_lasti_update(int opcodeIndex);
    void build_tuple(size_t argCnt);
    void extend_tuple(size_t argCnt);
    void build_list(size_t argCnt);
    void extend_list_recursively(Local list, size_t argCnt);
    void extend_list(size_t argCnt);
    void build_set(size_t argCnt);
    void unpack_ex(size_t size, int opcode);

    void build_map(size_t argCnt);

    Label getOffsetLabel(int jumpTo);
    void for_iter(int loopIndex, int opcodeIndex, BlockInfo *loopInfo);

    // Checks to see if we have a null value as the last value on our stack
    // indicating an error, and if so, branches to our current error handler.
    void error_check(const char* reason = nullptr);
    void int_error_check(const char* reason = nullptr);

    vector<Label>& get_raise_and_free_labels(size_t blockId);
    vector<Label>& get_reraise_and_free_labels(size_t blockId);
    void ensure_raise_and_free_locals(size_t localCount);
    void emit_raise_and_free(ExceptionHandler* handler);
    void spill_stack_for_raise(size_t localCount);

    void ensure_labels(vector<Label>& labels, size_t count);

    void branch_raise(const char* reason = nullptr);
    size_t clear_value_stack();
    void raise_on_negative_one();

    void clean_stack_for_reraise();

    void unwind_eh(ExceptionHandler* fromHandler, ExceptionHandler* toHandler = nullptr);

    __unused void unwind_loop(Local finallyReason, EhFlags branchKind, int branchOffset);

    ExceptionHandler * get_ehblock();

    void mark_offset_label(int index);

    // Frees our iteration temporary variable which gets allocated when we hit
    // a FOR_ITER.  Used when we're breaking from the current loop.
    void free_iter_local();

    void jump_absolute(size_t index, size_t from);

    // Frees all of the iteration variables in a range. Used when we're
    // going to branch to a finally through multiple loops.
    void free_all_iter_locals(size_t to = 0);

    // Frees all of our iteration variables.  Used when we're unwinding the function
    // on an exception.
    void free_iter_locals_on_exception();

    void dec_stack(size_t size = 1);

    void inc_stack(size_t size = 1, StackEntryKind kind = STACK_KIND_OBJECT);

    // Gets the next opcode skipping for EXTENDED_ARG
    int get_extended_opcode(int curByte);

    // Handles POP_JUMP_IF_FALSE/POP_JUMP_IF_TRUE with a possible error value on the stack.
    // If the value on the stack is -1, we branch to the current error handler.
    // Otherwise branches based if the current value is true/false based upon the current opcode
    void branch_or_error(int& i);

    // Handles POP_JUMP_IF_FALSE/POP_JUMP_IF_TRUE with a bool value known to be on the stack.
    // Branches based if the current value is true/false based upon the current opcode
    void branch(int& i);
    JittedCode* compile_worker();

    void periodic_work();
    void store_fast(int local, int opcodeIndex);

    void load_const(int constIndex, int opcodeIndex);

    void return_value(int opcodeIndex);

    void load_fast(int local, int opcodeIndex);
    void load_fast_worker(int local, bool checkUnbound);
    void unpack_sequence(size_t size, int opcode);

    __unused Local get_optimized_local(int index, AbstractValueKind kind);
    void pop_except();

    bool can_optimize_pop_jump(int opcodeIndex);
    void unary_positive(int opcodeIndex);
    void unary_negative(int opcodeIndex);
    void unary_not(int& opcodeIndex);

    void jump_if_or_pop(bool isTrue, int opcodeIndex, int offset);
    void pop_jump_if(bool isTrue, int opcodeIndex, int offset);
    void jump_if_not_exact(int opcodeIndex, int jumpTo);
    void test_bool_and_branch(Local value, bool isTrue, Label target);

    void debug_log(const char* fmt, ...);
};


#endif
