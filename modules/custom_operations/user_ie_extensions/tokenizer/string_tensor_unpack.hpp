// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <openvino/op/op.hpp>

// Unpack a string tensor representation regardless of the source format, which
// can be an OV tensor with element::string element type (if supported) or u8
// packed representation, to a decompose tensor representation that may potentially
// consist of multiple tensors. The destination format is defined by `mode` attribute.
// Shape of the output tensor is compitelly recognized from the input (if supported)
// or defined partially by a dedicated input attribute `shape`. If `shape` is not set,
// which default to completelly dynamic `shape`, then output shape is defined
// by an input tensor.
class StringTensorUnpack : public ov::op::Op {
public:
    OPENVINO_OP("StringTensorUnpack");

    StringTensorUnpack () = default;

    StringTensorUnpack(ov::OutputVector inputs, const std::string& mode = "begins_ends")
        : ov::op::Op(inputs), m_mode(mode) {
        constructor_validate_and_infer_types();
    }

    void validate_and_infer_types() override;

    std::shared_ptr<ov::Node> clone_with_new_inputs(const ov::OutputVector& inputs) const override {
        auto result = std::make_shared<StringTensorUnpack>(inputs, m_mode);
        return result;
    }

    bool visit_attributes(ov::AttributeVisitor& visitor) override {
        visitor.on_attribute("mode", m_mode);
        return true;
    }

    bool has_evaluate() const {
        return true;
    }

    bool evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const;

private:

    std::string m_mode = "begins_ends";
};