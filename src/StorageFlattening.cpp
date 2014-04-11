#include <sstream>

#include "StorageFlattening.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Scope.h"

namespace Halide {
namespace Internal {

using std::ostringstream;
using std::string;
using std::vector;
using std::map;

class FlattenDimensions : public IRMutator {
public:
    FlattenDimensions(const map<string, Function> &e) : env(e) {
    }
    Scope<int> scope;
private:
    const map<string, Function> &env;

    Expr flatten_args(const string &name, const vector<Expr> &args) {
        Expr idx = 0;
        vector<Expr> mins(args.size()), strides(args.size());

        for (size_t i = 0; i < args.size(); i++) {
            string dim = int_to_string(i);
            string stride_name = name + ".stride." + dim;
            string min_name = name + ".min." + dim;
            string stride_name_constrained = stride_name + ".constrained";
            string min_name_constrained = min_name + ".constrained";
            if (scope.contains(stride_name_constrained)) {
                stride_name = stride_name_constrained;
            }
            if (scope.contains(min_name_constrained)) {
                min_name = min_name_constrained;
            }
            strides[i] = Variable::make(Int(32), stride_name);
            mins[i] = Variable::make(Int(32), min_name);
        }

        if (env.find(name) != env.end()) {
            // f(x, y) -> f[(x-xmin)*xstride + (y-ymin)*ystride] This
            // strategy makes sense when we expect x to cancel with
            // something in xmin.  We use this for internal allocations
            for (size_t i = 0; i < args.size(); i++) {
                idx += (args[i] - mins[i]) * strides[i];
            }
        } else {
            // f(x, y) -> f[x*stride + y*ystride - (xstride*xmin +
            // ystride*ymin)]. The idea here is that the last term
            // will be pulled outside the inner loop. We use this for
            // external buffers, where the mins and strides are likely
            // to be symbolic
            Expr base = 0;
            for (size_t i = 0; i < args.size(); i++) {
                idx += args[i] * strides[i];
                base += mins[i] * strides[i];
            }
            idx -= base;
        }

        return idx;
    }

    using IRMutator::visit;

    void visit(const Realize *realize) {
        Stmt body = mutate(realize->body);

        // Since Allocate only handles one-dimensional arrays, we need another
        // means to populate buffer_t for intermediate realizations with
        // correct min/extent/stride values. These values are required when
        // dealing with kernel loops wich require information about the
        // dimensionality of a buffer.  We generate a create_buffer_t
        // intrinsic to populate the buffer in this case.
        vector<bool> make_buffer_t(realize->types.size());
        map<string, Function>::const_iterator it = env.find(realize->name);
        if (it != env.end()) {
	    const Schedule &sched = it->second.schedule();
            bool is_kernel_loop = false;
            for (size_t i = 0; i < sched.dims.size(); i++) {
                if (sched.dims[i].for_type == For::Kernel) {
                    is_kernel_loop = true;
                    break;
                }
            }
            if (is_kernel_loop) {
                for (size_t i = 0; i < realize->types.size(); i++)
                    make_buffer_t[i] = true;
            }
        }

        // Compute the size
        std::vector<Expr> extents;
        for (size_t i = 0; i < realize->bounds.size(); i++) {
          extents.push_back(realize->bounds[i].extent);
          extents[i] = mutate(extents[i]);
        }

        vector<int> storage_permutation;
        {
            map<string, Function>::const_iterator iter = env.find(realize->name);
            assert(iter != env.end() && "Realize node refers to function not in environment");
            const vector<string> &storage_dims = iter->second.schedule().storage_dims;
            const vector<string> &args = iter->second.args();
            for (size_t i = 0; i < storage_dims.size(); i++) {
                for (size_t j = 0; j < args.size(); j++) {
                    if (args[j] == storage_dims[i]) {
                        storage_permutation.push_back((int)j);
                    }
                }
                assert(storage_permutation.size() == i+1);
            }
        }

        assert(storage_permutation.size() == realize->bounds.size());

        stmt = body;
        for (size_t idx = 0; idx < realize->types.size(); idx++) {
            string buffer_name = realize->name;
            if (realize->types.size() > 1) {
                buffer_name = buffer_name + '.' + int_to_string(idx);
            }

            // Make the names for the mins, extents, and strides
            int dims = realize->bounds.size();
            vector<string> min_name(dims), extent_name(dims), stride_name(dims);
            for (int i = 0; i < dims; i++) {
                string d = int_to_string(i);
                min_name[i] = buffer_name + ".min." + d;
                stride_name[i] = buffer_name + ".stride." + d;
                extent_name[i] = buffer_name + ".extent." + d;
            }
            vector<Expr> min_var(dims), extent_var(dims), stride_var(dims);
            for (int i = 0; i < dims; i++) {
                min_var[i] = Variable::make(Int(32), min_name[i]);
                extent_var[i] = Variable::make(Int(32), extent_name[i]);
                stride_var[i] = Variable::make(Int(32), stride_name[i]);
            }
            // Promote the type to be a multiple of 8 bits
            Type t = realize->types[idx];
            t.bits = t.bytes() * 8;

            // Make the allocation node
            stmt = Allocate::make(buffer_name, t, extents, stmt);

            // Create a buffer_t object if necessary
            if (make_buffer_t[idx]) {
                vector<Expr> args(dims*3 + 2);
                args[0] = Call::make(Handle(), Call::null_handle, vector<Expr>(), Call::Intrinsic);
                args[1] = realize->types[idx].bytes();
                for (int i = 0; i < dims; i++) {
                    args[3*i+2] = min_var[i];
                    args[3*i+3] = extent_var[i];
                    args[3*i+4] = stride_var[i];
                }
                Expr buf = Call::make(Handle(), Call::create_buffer_t,
                                      args, Call::Intrinsic);
                stmt = LetStmt::make(buffer_name + ".buffer",
                                     buf,
                                     stmt);
            }

            // Compute the strides
            for (int i = (int)realize->bounds.size()-1; i > 0; i--) {
                int prev_j = storage_permutation[i-1];
                int j = storage_permutation[i];
                Expr stride = stride_var[prev_j] * extent_var[prev_j];
                stmt = LetStmt::make(stride_name[j], stride, stmt);
            }
            // Innermost stride is one
            if (dims > 0) {
                int innermost = storage_permutation.empty() ? 0 : storage_permutation[0];
                stmt = LetStmt::make(stride_name[innermost], 1, stmt);
            }

            // Assign the mins and extents stored
            for (size_t i = realize->bounds.size(); i > 0; i--) {
                stmt = LetStmt::make(min_name[i-1], realize->bounds[i-1].min, stmt);
                stmt = LetStmt::make(extent_name[i-1], realize->bounds[i-1].extent, stmt);
            }
        }
    }

    void visit(const Provide *provide) {

        vector<Expr> values(provide->values.size());
        for (size_t i = 0; i < values.size(); i++) {
            values[i] = mutate(provide->values[i]);

            // Promote the type to be a multiple of 8 bits
            Type t = values[i].type();
            t.bits = t.bytes() * 8;
            if (t.bits != values[i].type().bits) {
                values[i] = Cast::make(t, values[i]);
            }
        }

        if (values.size() == 1) {
            Expr idx = mutate(flatten_args(provide->name, provide->args));
            stmt = Store::make(provide->name, values[0], idx);
        } else {

            vector<string> names(provide->values.size());
            Stmt result;

            // Store the values by name
            for (size_t i = 0; i < provide->values.size(); i++) {
                string name = provide->name + "." + int_to_string(i);
                names[i] = name + ".value";
                Expr var = Variable::make(values[i].type(), names[i]);

                Stmt store;

                Expr idx = mutate(flatten_args(name, provide->args));
                store = Store::make(name, var, idx);

                if (result.defined()) {
                    result = Block::make(result, store);
                } else {
                    result = store;
                }
            }

            // Add the let statements that define the values
            for (size_t i = provide->values.size(); i > 0; i--) {
                result = LetStmt::make(names[i-1], values[i-1], result);
            }

            stmt = result;
        }
    }

    void visit(const Call *call) {

        if (call->call_type == Call::Extern || call->call_type == Call::Intrinsic) {
            vector<Expr> args(call->args.size());
            bool changed = false;
            for (size_t i = 0; i < args.size(); i++) {
                args[i] = mutate(call->args[i]);
                if (!args[i].same_as(call->args[i])) changed = true;
            }
            if (!changed) {
                expr = call;
            } else {
                expr = Call::make(call->type, call->name, args, call->call_type);
            }
        } else {
            string name = call->name;
            if (call->call_type == Call::Halide &&
                call->func.outputs() > 1) {
                name = name + '.' + int_to_string(call->value_index);
            }

            // Promote the type to be a multiple of 8 bits
            Type t = call->type;
            t.bits = t.bytes() * 8;

            Expr idx = mutate(flatten_args(name, call->args));
            expr = Load::make(t, name, idx, call->image, call->param);

            if (call->type.bits != t.bits) {
                expr = Cast::make(call->type, expr);
            }
        }
    }

    void visit(const LetStmt *let) {
        // Discover constrained versions of things.
        bool constrained_version_exists = ends_with(let->name, ".constrained");
        if (constrained_version_exists) {
            scope.push(let->name, 0);
        }

        IRMutator::visit(let);

        if (constrained_version_exists) {
            scope.pop(let->name);
        }
    }
};

class CreateKernelLoads : public IRMutator {
public:
    CreateKernelLoads() {
        inside_kernel_loop = false;
    }
    Scope<int> scope;
    bool inside_kernel_loop;
private:
    using IRMutator::visit;

    void visit(const Provide *provide) {
        if (!inside_kernel_loop) {
            IRMutator::visit(provide);
            return;
        }

        vector<Expr> values(provide->values.size());
        for (size_t i = 0; i < values.size(); i++) {
            values[i] = mutate(provide->values[i]);

            // Promote the type to be a multiple of 8 bits
            Type t = values[i].type();
            t.bits = t.bytes() * 8;
            if (t.bits != values[i].type().bits) {
                values[i] = Cast::make(t, values[i]);
            }
        }

        if (values.size() == 1) {
            stmt = Store::make(provide->name, values[0], provide->args);
        } else {

            vector<string> names(provide->values.size());
            Stmt result;

            // Store the values by name
            for (size_t i = 0; i < provide->values.size(); i++) {
                string name = provide->name + "." + int_to_string(i);
                names[i] = name + ".value";
                Expr var = Variable::make(values[i].type(), names[i]);
                Stmt store = Store::make(name, var, provide->args);
                if (result.defined()) {
                    result = Block::make(result, store);
                } else {
                    result = store;
                }
            }

            // Add the let statements that define the values
            for (size_t i = provide->values.size(); i > 0; i--) {
                result = LetStmt::make(names[i-1], values[i-1], result);
            }

            stmt = result;
        }
    }

    void visit(const Call *call) {
        if (!inside_kernel_loop) {
            IRMutator::visit(call);
            return;
        }

        string name = call->name;
        if (call->call_type == Call::Halide &&
            call->func.outputs() > 1) {
            name = name + '.' + int_to_string(call->value_index);
        }

        vector<Expr> idx(call->args.size());
        for (size_t i = 0; i < idx.size(); i++) {
            string d = int_to_string(i);
            string min_name = name + ".min." + d;
            string min_name_constrained = min_name + ".constrained";
            if (scope.contains(min_name_constrained)) {
                min_name = min_name_constrained;
            }
            string extent_name = name + ".extent." + d;
            string extent_name_constrained = extent_name + ".constrained";
            if (scope.contains(extent_name_constrained)) {
                extent_name = extent_name_constrained;
            }

            Expr min = Variable::make(Int(32), min_name);
            Expr extent = Variable::make(Int(32), extent_name);
            idx[i] = (call->args[i] - min);

            // Normalize the two spatial coordinates x,y
            if (i < 2) {
                idx[i] = (Cast::make(Float(32), idx[i]) + 0.5f) / extent;
            }
        }

        expr = Load::make(call->type, name, idx, call->image, call->param);
    }

    void visit(const LetStmt *let) {
        // Discover constrained versions of things.
        bool constrained_version_exists = ends_with(let->name, ".constrained");
        if (constrained_version_exists) {
            scope.push(let->name, 0);
        }

        IRMutator::visit(let);

        if (constrained_version_exists) {
            scope.pop(let->name);
        }
    }

    void visit(const For *loop) {
        bool old_kernel_loop = inside_kernel_loop;
        if (loop->for_type == For::Kernel)
            inside_kernel_loop = true;
        IRMutator::visit(loop);
        inside_kernel_loop = old_kernel_loop;
    }
};




Stmt storage_flattening(Stmt s, const map<string, Function> &env) {
    s = CreateKernelLoads().mutate(s);

    return FlattenDimensions(env).mutate(s);
}

}
}
