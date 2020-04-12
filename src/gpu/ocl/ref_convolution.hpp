/*******************************************************************************
* Copyright 2019-2020 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_OCL_REF_CONVOLUTION_HPP
#define GPU_OCL_REF_CONVOLUTION_HPP

#include "common/c_types_map.hpp"
#include "common/primitive.hpp"
#include "gpu/gpu_primitive.hpp"

#include "gpu/compute/compute.hpp"
#include "gpu/gpu_convolution_pd.hpp"
#include "gpu/ocl/ocl_resource.hpp"
#include "gpu/ocl/ocl_stream.hpp"
#include "gpu/primitive_conf.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

struct ref_convolution_fwd_t : public gpu_primitive_t {
    struct pd_t : public gpu_convolution_fwd_pd_t {
        using gpu_convolution_fwd_pd_t::gpu_convolution_fwd_pd_t;

        DECLARE_COMMON_PD_T("ocl:ref:any", ref_convolution_fwd_t);

        status_t init(engine_t *engine) {
            using namespace data_type;

            const auto *compute_engine
                    = utils::downcast<compute::compute_engine_t *>(engine);

            const auto attr_skip_mask
                    = primitive_attr_t::skip_mask_t::oscale_runtime
                    | primitive_attr_t::skip_mask_t::post_ops;

            bool ok = set_default_alg_kind(alg_kind::convolution_direct)
                    && utils::one_of(desc()->prop_kind,
                            prop_kind::forward_training,
                            prop_kind::forward_inference)
                    && desc()->alg_kind == alg_kind::convolution_direct
                    && IMPLICATION(
                            utils::one_of(f16, src_md_.data_type,
                                    weights_md_.data_type, dst_md_.data_type),
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp16))
                    && this->set_default_formats()
                    && attr()->has_default_values(attr_skip_mask)
                    && post_ops_ok(attr())
                    && IMPLICATION(!attr()->output_scales_.has_default_values(),
                            utils::one_of(src_md_.data_type, s8, u8)
                                    && utils::one_of(
                                            attr()->output_scales_.mask_, 0,
                                            1 << 1));
            if (!ok) return status::unimplemented;

            auto scales_status = init_scales_md();
            if (scales_status != status::success) return scales_status;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        const memory_desc_t *scales_md() const { return &scales_md_; }

        bool with_eltwise(int position) const {
            return attr()->post_ops_.contain(primitive_kind::eltwise, position);
        }

        bool with_sum() const {
            return attr()->post_ops_.find(primitive_kind::sum) != -1;
        }

        float eltwise_alpha() const {
            const int eltwise_idx
                    = attr()->post_ops_.find(primitive_kind::eltwise);
            return eltwise_idx != -1
                    ? attr()->post_ops_.entry_[eltwise_idx].eltwise.alpha
                    : 1.0f;
        }

        float eltwise_beta() const {
            const int eltwise_idx
                    = attr()->post_ops_.find(primitive_kind::eltwise);
            return eltwise_idx != -1
                    ? attr()->post_ops_.entry_[eltwise_idx].eltwise.beta
                    : 0.0f;
        }

        float eltwise_scale() const {
            const int eltwise_idx
                    = attr()->post_ops_.find(primitive_kind::eltwise);
            return eltwise_idx != -1
                    ? attr()->post_ops_.entry_[eltwise_idx].eltwise.scale
                    : 1.0f;
        }

        float sum_scale() const {
            const int sum_idx = attr()->post_ops_.find(primitive_kind::sum);
            return sum_idx != -1 ? attr()->post_ops_.entry_[sum_idx].sum.scale
                                 : 0.0f;
        }

        alg_kind_t eltwise_alg_kind() const {
            const int eltwise_idx
                    = attr()->post_ops_.find(primitive_kind::eltwise);
            return eltwise_idx != -1
                    ? attr()->post_ops_.entry_[eltwise_idx].eltwise.alg
                    : dnnl_alg_kind_undef;
        }

        bool with_scales() const {
            return !attr()->output_scales_.has_default_values();
        }

        bool with_common_scales() const {
            return with_scales() && attr()->output_scales_.mask_ == 0;
        }

        bool with_per_oc_scales() const {
            return with_scales() && attr()->output_scales_.mask_ == (1 << 1);
        }

        bool with_runtime_scales() const {
            return with_per_oc_scales() && !attr()->output_scales_.defined();
        }

        conv_conf_t conf;
        offsets_t off;

    private:
        bool set_default_formats() {
            using namespace format_tag;
            auto dat_tag = utils::pick(ndims() - 3, ncw, nchw, ncdhw);
            auto wei_tag = with_groups()
                    ? utils::pick(ndims() - 3, goiw, goihw, goidhw)
                    : utils::pick(ndims() - 3, oiw, oihw, oidhw);
            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }

        status_t init_scales_md() {
            if (with_per_oc_scales() && !with_runtime_scales()) {
                scales_md_.data_type = data_type::f32;
                scales_md_.ndims = 1;
                scales_md_.dims[0] = attr()->output_scales_.count_;
                return memory_desc_init_by_tag(scales_md_, format_tag::x);
            }

            return status::success;
        }

        memory_desc_t scales_md_;
    };

    ref_convolution_fwd_t(const pd_t *apd) : gpu_primitive_t(apd) {}

    status_t init(engine_t *engine) override {
        auto *compute_engine
                = utils::downcast<compute::compute_engine_t *>(engine);
        compute::kernel_ctx_t kernel_ctx;

        auto status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        compute_engine->create_binary(
                &binary_, "ref_convolution_fwd", kernel_ctx);
        if (!binary_) return status::runtime_error;

        return status::success;
    }

    status_t create_resource(
            engine_t *engine, resource_mapper_t &mapper) const override {
        if (mapper.has_resource(this)) return status::success;
        auto r = utils::make_unique<ocl_resource_t>();
        if (!r) return status::out_of_memory;
        if (pd()->with_per_oc_scales() && !pd()->with_runtime_scales()) {
            memory_desc_wrapper scales_mdw(pd()->scales_md());
            memory_storage_t *tmp_mem_storage_ptr;
            CHECK(engine->create_memory_storage(
                    &tmp_mem_storage_ptr, scales_mdw.nelems() * sizeof(float)));

            std::unique_ptr<memory_storage_t> tmp_mem_storage(
                    tmp_mem_storage_ptr);
            void *scales_ptr = nullptr;
            CHECK(tmp_mem_storage->map_data(&scales_ptr));
            utils::array_copy((float *)scales_ptr,
                    pd()->attr()->output_scales_.scales_,
                    pd()->attr()->output_scales_.count_);
            CHECK(tmp_mem_storage->unmap_data(scales_ptr));
            r->add_memory_storage(SCALES_, std::move(tmp_mem_storage));
        }
        CHECK(r->create_kernel_and_add(engine, binary_));
        mapper.add(this, std::move(r));
        return status::success;
    }

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        return execute_forward(ctx);
    }

private:
    status_t execute_forward(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::binary_t binary_;
    enum { SCALES_ = 0 };
};

struct ref_convolution_bwd_data_t : public gpu_primitive_t {
    struct pd_t : public gpu_convolution_bwd_data_pd_t {
        using gpu_convolution_bwd_data_pd_t::gpu_convolution_bwd_data_pd_t;

        DECLARE_COMMON_PD_T("ocl:ref:any", ref_convolution_bwd_data_t);

        status_t init(engine_t *engine) {
            bool ok = set_default_alg_kind(alg_kind::convolution_direct)
                    && desc()->prop_kind == prop_kind::backward_data
                    && desc()->alg_kind == alg_kind::convolution_direct
                    && this->set_default_formats()
                    && attr()->has_default_values();
            if (!ok) return status::unimplemented;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        conv_conf_t conf;
        offsets_t off;

    private:
        bool set_default_formats() {
            using namespace format_tag;
            auto dat_tag = utils::pick(ndims() - 3, ncw, nchw, ncdhw);
            auto wei_tag = with_groups()
                    ? utils::pick(ndims() - 3, goiw, goihw, goidhw)
                    : utils::pick(ndims() - 3, oiw, oihw, oidhw);
            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }
    };

    ref_convolution_bwd_data_t(const pd_t *apd) : gpu_primitive_t(apd) {}

    status_t init(engine_t *engine) override {
        auto *compute_engine
                = utils::downcast<compute::compute_engine_t *>(engine);
        compute::kernel_ctx_t kernel_ctx;

        auto status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        compute_engine->create_binary(
                &binary_, "ref_convolution_bwd_data", kernel_ctx);
        if (!binary_) return status::runtime_error;

        return status::success;
    }

    status_t create_resource(
            engine_t *engine, resource_mapper_t &mapper) const override {
        if (mapper.has_resource(this)) return status::success;
        auto r = utils::make_unique<ocl_resource_t>();
        if (!r) return status::out_of_memory;
        CHECK(r->create_kernel_and_add(engine, binary_));
        mapper.add(this, std::move(r));
        return status::success;
    }

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        return execute_backward_data(ctx);
    }

private:
    status_t execute_backward_data(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::binary_t binary_;
};

struct ref_convolution_bwd_weights_t : public gpu_primitive_t {
    struct pd_t : public gpu_convolution_bwd_weights_pd_t {
        using gpu_convolution_bwd_weights_pd_t::
                gpu_convolution_bwd_weights_pd_t;

        DECLARE_COMMON_PD_T("ocl:ref:any", ref_convolution_bwd_weights_t);

        status_t init(engine_t *engine) {
            bool ok = set_default_alg_kind(alg_kind::convolution_direct)
                    && desc()->prop_kind == prop_kind::backward_weights
                    && desc()->alg_kind == alg_kind::convolution_direct
                    && this->set_default_formats()
                    && attr()->has_default_values();
            if (!ok) return status::unimplemented;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        conv_conf_t conf;
        offsets_t off;

    private:
        bool set_default_formats() {
            using namespace format_tag;
            auto dat_tag = utils::pick(ndims() - 3, ncw, nchw, ncdhw);
            auto wei_tag = with_groups()
                    ? utils::pick(ndims() - 3, goiw, goihw, goidhw)
                    : utils::pick(ndims() - 3, oiw, oihw, oidhw);
            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }
    };

    ref_convolution_bwd_weights_t(const pd_t *apd) : gpu_primitive_t(apd) {}

    status_t init(engine_t *engine) override {
        auto *compute_engine
                = utils::downcast<compute::compute_engine_t *>(engine);
        compute::kernel_ctx_t kernel_ctx;

        auto status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        compute_engine->create_binary(
                &binary_, "ref_convolution_bwd_weights", kernel_ctx);
        if (!binary_) return status::runtime_error;

        return status::success;
    }

    status_t create_resource(
            engine_t *engine, resource_mapper_t &mapper) const override {
        if (mapper.has_resource(this)) return status::success;
        auto r = utils::make_unique<ocl_resource_t>();
        if (!r) return status::out_of_memory;
        CHECK(r->create_kernel_and_add(engine, binary_));
        mapper.add(this, std::move(r));
        return status::success;
    }

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        return execute_backward_weights(ctx);
    }

private:
    status_t execute_backward_weights(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::binary_t binary_;
};

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl
#endif
