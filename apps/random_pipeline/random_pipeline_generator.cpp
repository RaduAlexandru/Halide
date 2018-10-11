#include "Halide.h"
#include <iostream>
#include <random>

using namespace Halide;
using std::vector;

namespace {

// Convert a vector of Vars to Exprs. Useful for generating references
// to Funcs.
vector<Expr> make_arguments(vector<Var> vars) {
    vector<Expr> result;
    for (Var i : vars) {
        result.push_back(i);
    }
    return result;
}

// Generator to produce a random pipeline. The generated pipeline will
// be solely a function of the seed and the number of stages.
class RandomPipeline : public Halide::Generator<RandomPipeline> {
public:
    // The random seed to use to generate the pipeline.
    GeneratorParam<int> seed{"seed", 1};
    // The approximate max number of stages to generate in the random pipeline.
    GeneratorParam<int> max_stages{"max_stages", 20};
    GeneratorParam<int> max_channels{"max_channels", 2048};

    Input<Buffer<float>>  input{"input", 3};
    Output<Buffer<float>> output{"output", 3};

    std::mt19937 rng;

    // Helpers to generate random values.
    int rand_int(int min, int max) { return (rng() % (max - min + 1)) + min; }
    bool rand_bool() { return rng() % 2 == 0; }
    float rand_float() { return rand_int(0, 1 << 30) / (float)(1 << 30); }

    Expr rand_value(Type t) {
        if (t.is_int()) {
            return cast(t, rand_int(-128, 127));
        } else if (t.is_float()) {
            return cast(t, rand_float());
        } else {
            // Shouldn't get here.
            assert(false);
            return undef(t);
        }
    }

    struct Stage {
        Func func;
        int w, h, c; // approx width and height and channels; TODO: ADD 4TH DIMENSION FOR BATCH SIZE
    };

    // Generate a random convolution of one dimension of f, statically unrolled.
    Stage convolve(Stage f, int dim, int kernel_min, int kernel_max) {
        std::cout << "Convolving dimension " << dim
                  << " with kernel [" << kernel_min << ", " << kernel_max << "]\n";

        vector<Var> args = f.func.args();

        Expr def = cast(f.func.value().type(), 0);
        for (int i = kernel_min; i <= kernel_max; i++) {
            vector<Expr> coords = make_arguments(f.func.args());
            coords[dim] += i;
            def = def + rand_value(f.func.value().type()) * f.func(coords);
        }

        Func conv("conv_" + args[dim].name());
        conv(args) = def;

        return {conv, f.w, f.h, f.c};
    }

    // Generate a random convolution of one dimension of f using a reduction.
    Stage convolve_r(Stage f, int dim, int kernel_min, int kernel_max) {
        std::cout << "Convolving dimension " << dim
                  << " with kernel [" << kernel_min << ", " << kernel_max << "]"
                  << " using +=\n";

        vector<Var> args = f.func.args();

        Func conv("conv_r_" + args[dim].name());
        RDom r(kernel_min, kernel_max - kernel_min + 1);
        vector<Expr> coords = make_arguments(f.func.args());
        coords[dim] += r;
        conv(args) += rand_value(f.func.value().type()) * f.func(coords);

        return {conv, f.w, f.h, f.c};
    }

    // Generate a random convolution of one dimension of f using a reduction with a wrapper
    Stage convolve_w(Stage f, int dim, int kernel_min, int kernel_max) {
        std::cout << "Convolving dimension " << dim
                  << " with kernel [" << kernel_min << ", " << kernel_max << "]"
                  << " using sum() helper\n";

        vector<Var> args = f.func.args();

        Func conv("conv_w_" + args[dim].name());
        RDom r(kernel_min, kernel_max - kernel_min + 1);
        vector<Expr> coords = make_arguments(f.func.args());
        coords[dim] += r;
        conv(args) = sum(rand_value(f.func.value().type()) * f.func(coords));

        return {conv, f.w, f.h, f.c};
    }

    /*****
     * convolutional net type layers
     *****/

    // 50% chance of returning a pooling stage 50% chance returning 2D convolution
    Stage convolve_or_pool(Stage f, int kernel_min, int kernel_max) {
        if (rand_int(0,1) && f.w > 20 && f.h > 20) { // don't downsample if image too small
            int pool_type = rand_int(0,2);
            if (pool_type == 0) return pool2D(f);
            if (pool_type == 1) return pool2D_w(f);
            else return pool2D_r(f);
        } else {
            int conv_type = rand_int(0,2);
            if (conv_type == 0) return convolve2D(f, kernel_min, kernel_max);
            if (conv_type == 1) return convolve2D_w(f, kernel_min, kernel_max);
            else return convolve2D_r(f, kernel_min, kernel_max);
        }
    }

    /*** pooling stages ***/
    Stage pool2D(Stage f) { // for now always do 3x3 pool with stride 2
        std::cout << "Pooling 3x3 stride 2\n";
        vector<Var> args = f.func.args();
        Func pooled2D("pooled2D" + args[0].name() + args[1].name());

        int kernel_min = -1;
        int kernel_max = 1;
        int factor = 2;
        int scale = (kernel_max - kernel_min) * (kernel_max - kernel_min);

        Expr def = cast(f.func.value().type(), 0);

        // assuming input is 3d: w, h, c
        for (int i = kernel_min; i <= kernel_max; i++) {
            for (int j = kernel_min; j <= kernel_max; j++) {
                vector<Expr> pooled_coords = make_arguments(f.func.args());
                pooled_coords[0] = pooled_coords[0] * factor + i;
                pooled_coords[1] = pooled_coords[1] * factor + j;
                def = (def + f.func(pooled_coords)) / scale;
            }
        }

        pooled2D(args) = def;

        return {pooled2D, (f.w - 1)/factor, (f.h - 1)/factor, f.c};
    }

    // Generate a 3x3 pool with stride 2 of f using a reduction.
    Stage pool2D_r(Stage f) {
        std::cout << "Pooling 3x3 stride 2\n";
        vector<Var> args = f.func.args();
        Func pooled2D_r("pool2D_r_" + args[0].name() + args[1].name());

        int kernel_min = -1;
        int kernel_max = 1;
        int factor = 2;
        int scale = (kernel_max - kernel_min) * (kernel_max - kernel_min);

        RDom r(kernel_min, kernel_max - kernel_min + 1,
               kernel_min, kernel_max - kernel_min + 1);

        vector<Expr> coords = make_arguments(f.func.args());
        coords[0] = (coords[0] * factor + r.x) / scale;
        coords[1] = (coords[1] * factor + r.y) / scale;
        pooled2D_r(args) += f.func(coords);

        return {pooled2D_r, (f.w - 1)/factor, (f.h - 1)/factor, f.c};
    }

    // Generate a 3x3 pool with stride 2 of f using a reduction with a wrapper
    Stage pool2D_w(Stage f) {
        std::cout << "Pooling 3x3 stride 2 using sum() helper\n";
        vector<Var> args = f.func.args();
        Func pooled2D_w("pooled2D_w_" + args[0].name() + args[1].name());

        int kernel_min = -1;
        int kernel_max = 1;
        int factor = 2;
        int scale = (kernel_max - kernel_min) * (kernel_max - kernel_min);

        RDom r(kernel_min, kernel_max - kernel_min + 1,
               kernel_min, kernel_max - kernel_min + 1);

        vector<Expr> coords = make_arguments(f.func.args());
        coords[0] = (coords[0] * factor + r.x) / scale;
        coords[1] = (coords[1] * factor + r.y) / scale;
        pooled2D_w(args) = sum(f.func(coords));

        return {pooled2D_w, (f.w - 1)/factor, (f.h - 1)/factor, f.c};
    }

    /******* set of 2 dimensional (non separable) convs *********/
    // Generate a random convolution of one dimension of f, statically unrolled.
    Stage convolve2D(Stage f, int kernel_min, int kernel_max) {
        std::cout << "Convolving 2D dimension 1: " << 0
                  << " dimension 2: " << 1
                  << " with kernel [" << kernel_min << ", " << kernel_max << "]\n";

        vector<Var> args = f.func.args();

        Expr def = cast(f.func.value().type(), 0);

        // assuming input is 3d: w, h, c
        for (int c = 0; c < f.c; c++)  {
            for (int i = kernel_min; i <= kernel_max; i++) {
                for (int j = kernel_min; j <= kernel_max; j++) {
                    vector<Expr> coords = make_arguments(f.func.args());
                    coords[0] += i;
                    coords[1] += j;
                    coords[2] += c;
                    def = def + rand_value(f.func.value().type()) * f.func(coords);
                }
            }
        }

        Func conv("conv2D_" + args[0].name() + args[1].name());
        conv(args) = def;

        // choose a channel output size - 0.5 prob of doubling channel dim
        int channels_out = min(max_channels, std::max(128, f.c * rand_int(1,2)));
        return {conv, f.w, f.h, channels_out};
    }

    // Generate a random convolution of one dimension of f using a reduction.
    Stage convolve2D_r(Stage f, int kernel_min, int kernel_max) {
        std::cout << "Convolving 2D dimension 1: " << 0
                  << " dimension 2: " << 1
                  << " with kernel [" << kernel_min << ", " << kernel_max << "]"
                  << " using +=\n";

        vector<Var> args = f.func.args();

        Func conv("conv2D_r_" + args[0].name() + args[1].name());
        RDom r(kernel_min, kernel_max - kernel_min + 1,
               kernel_min, kernel_max - kernel_min + 1,
               0, f.c);
        vector<Expr> coords = make_arguments(f.func.args());
        coords[0] += r.x;
        coords[1] += r.y;
        coords[2] += r.z;
        conv(args) += rand_value(f.func.value().type()) * f.func(coords);

        // choose a channel output size - 0.5 prob of doubling channel dim
        int channels_out = min(max_channels, std::max(128, f.c * rand_int(1,2)));
        return {conv, f.w, f.h, channels_out};
    }

    // Generate a random convolution of one dimension of f using a reduction with a wrapper
    Stage convolve2D_w(Stage f, int kernel_min, int kernel_max) {
        std::cout << "Convolving 2D dimension 1: " << 0
                  << " dimension 2: " << 1
                  << " with kernel [" << kernel_min << ", " << kernel_max << "]"
                  << " using sum() helper\n";

        vector<Var> args = f.func.args();

        Func conv("conv2D_w_" + args[0].name() + args[1].name());
        RDom r(kernel_min, kernel_max - kernel_min + 1,
               kernel_min, kernel_max - kernel_min + 1);
        vector<Expr> coords = make_arguments(f.func.args());
        coords[0] += r.x;
        coords[1] += r.y;
        coords[2] += r.z;
        conv(args) = sum(rand_value(f.func.value().type()) * f.func(coords));

        // choose a channel output size - 0.5 prob of doubling channel dim
        int channels_out = min(max_channels, std::max(128, f.c * rand_int(1,2)));
        return {conv, f.w, f.h, channels_out};
    }


    // Generate an upsampling or downsampling of dimension dim by factor.
    Stage upsample(Stage f, int dim, int factor) {
        std::cout << "Upsampling dimension " << dim << " by " << factor << "x\n";

        vector<Expr> resampled_coords = make_arguments(f.func.args());
        resampled_coords[dim] = resampled_coords[dim] / factor;

        Func resampled("upsampled_" + f.func.args()[dim].name());
        resampled(f.func.args()) = f.func(resampled_coords);

        Stage s {resampled, f.w, f.h, f.c};
        if (dim == 0) {
            s.w *= factor;
        } else if (dim == 1) {
            s.h *= factor;
        } else {
            assert(false);
        }
        return s;
    }

    Stage downsample(Stage f, int dim, int factor) {
        std::cout << "Downsampling dimension " << dim << " by " << factor << "x\n";

        vector<Expr> resampled_coords = make_arguments(f.func.args());
        resampled_coords[dim] = resampled_coords[dim] * factor;

        Func resampled("downsampled_" + f.func.args()[dim].name());
        resampled(f.func.args()) = f.func(resampled_coords);

        Stage s {resampled, f.w, f.h, f.c};
        if (dim == 0) {
            s.w = (s.w + factor - 1)/factor;
        } else if (dim == 1) {
            s.h = (s.h + factor - 1)/factor;
        } else {
            assert(false);
        }
        return s;
    }

    Stage binary_op(Stage f, Stage g) {
        Func binary("binary_op");
        int op_type = rand_int(0, 4); // + , -, *, /, %
        if (op_type == 0) {
            binary(f.func.args()) = f.func(f.func.args()) + g.func(f.func.args());
        } else if (op_type == 1) {
            binary(f.func.args()) = f.func(f.func.args()) - g.func(f.func.args());
        } else if (op_type == 2) {
            binary(f.func.args()) = f.func(f.func.args()) * g.func(f.func.args());
        } else if (op_type == 3) {
            binary(f.func.args()) = f.func(f.func.args()) / max(1, g.func(f.func.args()));
        } else {
            binary(f.func.args()) = f.func(f.func.args()) % g.func(f.func.args());
        }
        return {binary, f.w, f.h, f.c};
    }

    Stage unary_op(Stage f) {
        Func unary("unary_op");
        vector<Expr> coords = make_arguments(f.func.args());

        int op_type = rand_int(0,3); // exp, log, sqrt, sin
        if (op_type == 0) {
            unary(f.func.args()) = exp(f.func(coords));
        } else if (op_type == 1) {
            unary(f.func.args()) = log(f.func(coords));
        } else if (op_type == 2) {
            unary(f.func.args()) = sqrt(f.func(coords));
        } else {
            unary(f.func.args()) = sin(f.func(coords));
        }
        return {unary, f.w, f.h, f.c};
    }

    // Generate an all-to-all communication in dimension dim, statically unrolled.
    Stage all_to_all(Stage f, int dim) {
        std::cout << "All to all on dimension " << dim << '\n';

        vector<Expr> reduction_coords = make_arguments(f.func.args());
        Expr e = 0.f;
        for (int i = 0; i < f.c; i++) {
            reduction_coords[dim] = i;
            e += f.func(reduction_coords) * (i + 1) * (f.func.args()[dim] + 1);
        }

        Func all("all");
        all(f.func.args()) = e;

        return {all, f.w, f.h, f.c};
    }

    // Generate an all-to-all communication in dimension dim using an RDom
    Stage all_to_all_r(Stage f, int dim) {
        std::cout << "All to all on dimension " << dim << " using += \n";

        vector<Expr> reduction_coords = make_arguments(f.func.args());
        RDom r(0, f.c);
        reduction_coords[dim] = r;
        Func all("all_r");
        all(f.func.args()) += f.func(reduction_coords) * r * (f.func.args()[dim] + 1);

        return {all, f.w, f.h, f.c};
    }

    // Generate an all-to-all communication in dimension dim using an RDom with wrapper func
    Stage all_to_all_w(Stage f, int dim) {
        std::cout << "All to all on dimension " << dim << " using += \n";

        vector<Expr> reduction_coords = make_arguments(f.func.args());
        RDom r(0, f.c);
        reduction_coords[dim] = r;
        Func all("all_w");
        all(f.func.args()) = sum(f.func(reduction_coords) * r * (f.func.args()[dim] + 1));

        return {all, f.w, f.h, f.c};
     }

    // Generate a forwards-then-backwards scan along a dimension
    Stage scan(Stage f, int dim) {
        std::cout << "Scan on dimension " << dim << '\n';
        int extent = dim == 0 ? f.w : dim == 1 ? f.h : 3;
        RDom r(1, extent - 1);
        Func scan("scan_" + f.func.args()[dim].name());
        vector<Expr> coords = make_arguments(f.func.args());
        scan(coords) = f.func(coords);
        coords[dim] = r;
        vector<Expr> prev_coords = coords;
        prev_coords[dim] = r-1;
        scan(coords) += scan(prev_coords);
        // Now in reverse
        coords[dim] = extent - r - 1;
        prev_coords[dim] = extent - r;
        scan(coords) += scan(prev_coords);
        return {scan, f.w, f.h, f.c};
    }

    // Transpose
    Stage transpose(Stage f) {
        Func transpose("transpose");
        vector<Expr> coords = make_arguments(f.func.args());
        vector<Expr> swizzled_coords = coords;
        std::swap(swizzled_coords[0], swizzled_coords[1]);

        transpose(coords) = f.func(swizzled_coords);

        return {transpose, f.h, f.w, f.c};

    }

    // Generate a random stage using f as an input.
    Stage random_stage(const vector<Stage> &s) {
        int m = (int)s.size() - 1;
        int i2 = m > 0 ? rand_int(0, m - 1) : 0;
        int i1 = m > 0 ? rand_int(i2 + 1, m) : 0;
        Stage f = s[i1], g = s[i2];
        int stage_type = rand_int(0, 11);
        if (stage_type == 0) {
            int dim = rand_int(0, 1);
            int kernel_min = rand_int(-3, 0);
            int kernel_max = rand_int(0, 3);
            return convolve(f, dim, kernel_min, kernel_max);
        } else if (stage_type == 1) {
            int dim = rand_int(0, 1);
            int kernel_min = rand_int(-10, 0);
            int kernel_max = rand_int(0, 10);
            return convolve_r(f, dim, kernel_min, kernel_max);
        } else if (stage_type == 2) {
            int dim = rand_int(0, 1);
            int kernel_min = rand_int(-10, 0);
            int kernel_max = rand_int(0, 10);
            return convolve_w(f, dim, kernel_min, kernel_max);
        } else if (stage_type == 3) {
            int dim1 = rand_int(0, 1);
            int dim2 = 1 - dim1;
            int kernel_min = rand_int(-3, 0);
            int kernel_max = rand_int(0, 3);
            return convolve_or_pool(f, kernel_min, kernel_max);
        } else if (stage_type == 4) {
            // For now, only upsample dimensions 0 or 1.
            int dim = rand_int(0, 1);
            int factor = 2;
            if (f.w < 2000 && f.h < 2000) {
                return upsample(f, dim, factor);
            } else {
                return random_stage(s);
            }
        } else if (stage_type == 5) {
            // For now, only downsample dimensions 0 or 1.
            int dim = rand_int(0, 1);
            int factor = 2;
            if (f.w > 128 && f.h > 128) {
                return downsample(f, dim, factor);
            } else {
                return random_stage(s);
            }
        } else if (stage_type == 6) {
            int dim = 2;
            return all_to_all(f, dim);
        } else if (stage_type == 7) {
            int dim = 2;
            return all_to_all_r(f, dim);
        } else if (stage_type == 8) {
            int dim = 2;
            return all_to_all_w(f, dim);
        } else if (stage_type == 9) {
            int dim = rand_int(0, 2);
            return scan(f, dim);
        } else if (stage_type == 10 && false) {
            // TODO: transpose disabled for now because f(x, y) + f(y, x) totally breaks the bounds inference done by the autoscheduler.
            return transpose(f);
        } else if (stage_type == 11) {
            return unary_op(f);
        } else if (i1 != i2) {
            // binary op two equal resolution stages
            if (f.w == g.w && f.h == g.h) {
                return binary_op(f, g);
            } else {
                return random_stage(s);
            }
        } else {
            return random_stage(s);
        }
    }

    void generate() {
        Var x("x"), y("y"), c("c");

        Func first;
        first(x, y, c) = input(x, y, c);

        rng.seed((int)seed);

        vector<Stage> stages;
        // Assume input starts at ~2000x2000
        stages.emplace_back(Stage{first, 2000, 2000, 3});

        for (int i = 0; i < max_stages - 2; i++) {
            std::cout << "Approx size: " << stages.back().w << ", " << stages.back().h << "\n";
            Stage next = random_stage(stages);
            stages.push_back(next);
            if (!auto_schedule) {
                stages.back().func.compute_root().reorder(x, c, y).vectorize(x, 8).parallel(y, 8);
            }
        }

        Stage tail = stages.back();

        // Resample back to the correct resolution
        if (tail.w >= 2048) {
            tail = downsample(tail, 0, tail.w / 2000);
        } else if (tail.w < 512) {
            tail = upsample(tail, 0, 2000 / tail.w);
        }

        if (tail.h >= 2048) {
            tail = downsample(tail, 1, tail.h / 2000);
        } else if (tail.h < 512) { // KCMA: does this make sense for conv outputs?
            tail = upsample(tail, 1, 2000 / tail.h);
        }

        output(tail.func.args()) = tail.func(tail.func.args());

        if (!auto_schedule) {
            output.compute_root().reorder(x, c, y).vectorize(x, 8).parallel(y);
        }

        if (auto_schedule) {
            input.dim(0).set_bounds_estimate(0, 2000)
                .dim(1).set_bounds_estimate(0, 2000)
                .dim(2).set_bounds_estimate(0, 3);
            output.estimate(output.args()[0], 0, 2000);
            output.estimate(output.args()[1], 0, 2000);
            output.estimate(output.args()[2], 0, 3);

            output.dim(0).set_bounds(0, 2000);
            output.dim(1).set_bounds(0, 2000);
            output.dim(2).set_bounds(0, 3);
        }
    }
};

}  // namespace

HALIDE_REGISTER_GENERATOR(RandomPipeline, random_pipeline)