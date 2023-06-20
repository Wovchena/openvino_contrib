// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <functional>

#include "normalizer.h"
#include "sentence_piece.hpp"

#include "openvino/op/util/framework_node.hpp"
#include "openvino/opsets/opset10.hpp"

#include "fast_tokenizer/normalizers/normalizers.h"
#include "fast_tokenizer/models/models.h"
#include "fast_tokenizer/pretokenizers/pretokenizers.h"

// TODO: Replace shape_size(t.get_shape()) by t.get_size(), where t is ov::Tensor

#ifndef OPENVINO_ELEMENT_STRING_SUPPORTED
    #define OPENVINO_ELEMENT_STRING_SUPPORTED 0
#endif

#ifndef OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK
    #define OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK 0
#endif

#define USE_STRING_TENSORS 0    // modify this depending on willingness to use explicit string tensors

#if USE_STRING_TENSORS && !OPENVINO_ELEMENT_STRING_SUPPORTED
    #error "USE_STRING_TENSORS = 1 can be used only when OpenVINO supports element::string that is determined by OPENVINO_ELEMENT_STRING_SUPPORTED == 1"
#endif

#define SENTENCE_PIECE_EXTENSION_DECOMPOSED_STRINGS 0

using sentencepiece::SentencePieceProcessor;
using namespace TemplateExtension;
using namespace ov;
using namespace ov::frontend;
using namespace ov::opset10;

namespace {
    template<typename T>
    T extract_scalar_const_value(const std::shared_ptr<Node>& node, const std::string& const_name) {
        auto const_node = as_type_ptr<Constant>(node);
        FRONT_END_GENERAL_CHECK(const_node, "Conversion expects " + const_name + " to be constant.");
        std::vector<T> const_value = const_node->cast_vector<T>();
        FRONT_END_GENERAL_CHECK(const_value.size() == 1, "Conversion expects " + const_name + " to be a scalar.");
        return const_value[0];
    }
}  // namespace

SentencepieceTokenizer::SentencepieceTokenizer(const OutputVector& args, int32_t nbest_size, float alpha,
    bool add_bos, bool add_eos, bool reverse) : m_sp(std::make_shared<SentencePieceProcessor>()),
    m_nbest_size(nbest_size), m_alpha(alpha), m_add_bos(add_bos), m_add_eos(add_eos),
    m_reverse(reverse), Op(args) {
    auto sp_model_const = as_type_ptr<Constant>(args[0].get_node_shared_ptr());
    FRONT_END_GENERAL_CHECK(sp_model_const, "SentencepieceTokenizer expects SentencePiece model to be constant.");
    auto spm_model = static_cast<const char*>(sp_model_const->get_data_ptr());
    auto spm_model_size = sp_model_const->get_byte_size();

    // configure SentencePieceProcessor
    std::string model_proto(spm_model, spm_model_size);
    CHECK_OK(m_sp->LoadFromSerializedProto(model_proto));

    // form extra options to configure SentencePieceProcessor
    std::string extra_options = "";
    if (m_add_bos) {
        extra_options += "bos";
    }
    if (m_add_eos) {
        extra_options = extra_options.empty() ? extra_options : extra_options + ":";
        extra_options += "eos";
    }
    /* TODO: TF ignores this option, so we are ignoring it as well; need to understand what should we do
    if (m_reverse) {
        extra_options = extra_options.empty() ? extra_options : extra_options + ":";
        extra_options += "reverse";
    }
    */
    // example of extra_options, if "bos:eos:reverse"
    CHECK_OK(m_sp->SetEncodeExtraOptions(extra_options));
    constructor_validate_and_infer_types();
}

SentencepieceTokenizer::SentencepieceTokenizer(const OutputVector& args, const std::shared_ptr<sentencepiece::SentencePieceProcessor>& sp,
    int32_t nbest_size, float alpha, bool add_bos, bool add_eos, bool reverse) : m_sp(sp),
    m_nbest_size(nbest_size), m_alpha(alpha), m_add_bos(add_bos), m_add_eos(add_eos),
    m_reverse(reverse), Op(args) {
    constructor_validate_and_infer_types();
}

void SentencepieceTokenizer::validate_and_infer_types() {

    #if SENTENCE_PIECE_EXTENSION_DECOMPOSED_STRINGS

    FRONT_END_GENERAL_CHECK(get_input_size() == 1 + 3, "SentencepieceTokenizer expects 4 inputs: sp model and input sentences represented as 3 decomposed tensors (begins, ends, sybols)");
    FRONT_END_GENERAL_CHECK(get_input_element_type(0) == element::u8, "SentencepieceTokenizer accepts sp model as the first input and it should be of type u8 tensor");
    FRONT_END_GENERAL_CHECK(get_input_element_type(1) == element::i32, "SentencepieceTokenizer accepts begins offsets as the second and it should be of type i32 tensor");
    FRONT_END_GENERAL_CHECK(get_input_element_type(2) == element::i32, "SentencepieceTokenizer accepts ends offsets as the third and it should be of type i32 tensor");
    FRONT_END_GENERAL_CHECK(get_input_element_type(3) == element::u8, "SentencepieceTokenizer accepts sentence symbols as the fourth input and it should be of type u8 tensor");

    #else

    FRONT_END_GENERAL_CHECK(get_input_size() == 2, "SentencepieceTokenizer expects two inputs: sp model and input sentences");
    FRONT_END_GENERAL_CHECK(get_input_element_type(0) == element::u8, "SentencepieceTokenizer accepts sp model as the first input and it should be of type u8 tensor");

    #if USE_STRING_TENSORS

        #if OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK
        FRONT_END_GENERAL_CHECK(
            get_input_element_type(1) == element::string || get_input_element_type(1) == element::u8,
            "SentencepieceTokenizer accepts sentences as the second input and it should be of type u8 or string depending on the current stage of model preparation");
        #else
        FRONT_END_GENERAL_CHECK(
            get_input_element_type(1) == element::string,
            "SentencepieceTokenizer accepts sentences as the second input and it should be of type string tensor");
        #endif

    #else

#if 0   // change to 0 when compiled with master and the bug with data propagation from within inline context is not solved
    FRONT_END_GENERAL_CHECK(
        get_input_element_type(1) == element::u8,
        "SentencepieceTokenizer accepts sentences as the second input and it should be of type u8 tensor, but got " +
            get_input_element_type(1).get_type_name());
#endif

    #endif

    #endif

    // The operation SentencepieceTokenizerExtensionOp has three outputs: sparse indices, sparse values
    // and dense shape
    set_output_type(0, element::i64, PartialShape{ Dimension(), Dimension(2) });
    set_output_type(1, element::i32, PartialShape{ Dimension() });
    set_output_type(2, element::i64, PartialShape{ Dimension(2) });
}

bool SentencepieceTokenizer::visit_attributes(AttributeVisitor& visitor) {
    visitor.on_attribute("nbest_size", m_nbest_size);
    visitor.on_attribute("alpha", m_alpha);
    visitor.on_attribute("add_bos", m_add_bos);
    visitor.on_attribute("add_eos", m_add_eos);
    visitor.on_attribute("reverse", m_reverse);
    return true;
}

void parse_packed_strings (const Tensor& packed, int32_t& batch_size, const int32_t*& begin_ids, const int32_t*& end_ids, const uint8_t*& symbols) {
    auto strings = packed.data<const uint8_t>();
    auto bitstream_size = packed.get_byte_size();
    // check the format of the input bitstream representing the string tensor
    FRONT_END_GENERAL_CHECK(bitstream_size >= 4, "Incorrect packed string tensor format: no batch size in the packed string tensor");
    batch_size = *reinterpret_cast<const int32_t*>(strings + 0);
    FRONT_END_GENERAL_CHECK(bitstream_size >= 4 + 4 + 4 * batch_size,
        "Incorrect packed string tensor format: the packed string tensor must contain first string offset and end indices");
    begin_ids = reinterpret_cast<const int32_t*>(strings + 4);
    end_ids = begin_ids + 1;
    symbols = strings + 4 + 4 + 4 * batch_size;
}

bool SentencepieceTokenizer::evaluate(TensorVector& outputs, const TensorVector& inputs) const {
    std::vector<int64_t> sparse_indices;
    std::vector<int32_t> sparse_values;
    std::vector<int64_t> sparse_dense_shape;

#if SENTENCE_PIECE_EXTENSION_DECOMPOSED_STRINGS

    auto begin_ids = inputs[1].data<const int32_t>();
    auto end_ids = inputs[2].data<const int32_t>();
    auto data = inputs[3].data<const uint8_t>();

    auto batch_size = shape_size(inputs[1].get_shape());

#else

#if USE_STRING_TENSORS

    #if OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK
    const ov::Tensor& strings_tensor = **reinterpret_cast<ov::Tensor**>(inputs[1].data<uint8_t>());
    #else
    const ov::Tensor& strings_tensor = inputs[1];
    #endif

    const std::string* strings = strings_tensor.data<std::string>();
    size_t batch_size = ov::shape_size(strings_tensor.get_shape());

#else

    // const uint8_t* strings = inputs[1].data<const uint8_t>();
    // auto bitstream_size = inputs[1].get_byte_size();

    // // check the format of the input bitstream representing the string tensor
    // FRONT_END_GENERAL_CHECK(bitstream_size >= 4, "Incorrect packed string tensor format: no batch size in the packed string tensor");
    // auto batch_size = *reinterpret_cast<const int32_t*>(strings + 0);
    // FRONT_END_GENERAL_CHECK(bitstream_size >= 4 + 4 + 4 * batch_size,
    //     "Incorrect packed string tensor format: the packed string tensor must contain first string offset and end indices");
    // auto begin_ids = reinterpret_cast<const int32_t*>(strings + 4);
    // auto end_ids = begin_ids + 1;
    // auto data = strings + 4 + 4 + 4 * batch_size;
    int32_t batch_size;
    const int32_t* begin_ids;
    const int32_t* end_ids;
    const uint8_t* data;
    parse_packed_strings(inputs[1], batch_size, begin_ids, end_ids, data);

#endif

#endif
    //std::cerr << "    Batch size: " << batch_size << "\n";

    size_t max_token_id = 0;
    for (size_t batch_ind = 0; batch_ind < batch_size; ++batch_ind) {
#if USE_STRING_TENSORS && !SENTENCE_PIECE_EXTENSION_DECOMPOSED_STRINGS
        const std::string& sentence = strings[batch_ind];
        //std::cerr << "    sentence: " << sentence << "\n";
#else
        auto begin_ind = begin_ids[batch_ind];
        auto end_ind = end_ids[batch_ind];
        //std::string sentence(data + begin_ind, data + end_ind);
        absl::string_view sentence((const char*)data + begin_ind, end_ind - begin_ind);
        //std::cerr << "string: " << sentence << "\n";
#endif
        std::vector<int32_t> ids;
        CHECK_OK(m_sp->SampleEncode(sentence, m_nbest_size, m_alpha, &ids));
        // put into resulted vectors
        for (size_t token_id = 0; token_id < ids.size(); ++token_id) {
            sparse_indices.push_back(static_cast<int64_t>(batch_ind));
            sparse_indices.push_back(static_cast<int64_t>(token_id));
            sparse_values.push_back(static_cast<int32_t>(ids[token_id]));
        }
        max_token_id = max_token_id < ids.size() ? ids.size() : max_token_id;
    }
    sparse_dense_shape.push_back(static_cast<int64_t>(batch_size));
    sparse_dense_shape.push_back(static_cast<int64_t>(max_token_id));

    outputs[0].set_shape({ sparse_indices.size() / 2, 2 });
    memcpy(outputs[0].data(), sparse_indices.data(), sizeof(int64_t) * sparse_indices.size());
    outputs[1].set_shape({ sparse_values.size() });
    memcpy(outputs[1].data(), sparse_values.data(), sizeof(int32_t) * sparse_values.size());
    outputs[2].set_shape({ 2 });
    memcpy(outputs[2].data(), sparse_dense_shape.data(), sizeof(int64_t) * sparse_dense_shape.size());
    return true;
}

bool SentencepieceTokenizer::has_evaluate() const {
    return true;
}

std::shared_ptr<Node> SentencepieceTokenizer::clone_with_new_inputs(const OutputVector& new_args) const {
    return std::make_shared<SentencepieceTokenizer>(new_args, m_sp, m_nbest_size, m_alpha, m_add_bos, m_add_eos, m_reverse);
}

OutputVector translate_sentencepiece_op(const NodeContext& node) {
    // extract model to configure SentencePieceTokenizer
    auto sp_model_ov_any = node.get_attribute_as_any("model");
    FRONT_END_GENERAL_CHECK(sp_model_ov_any.is<std::string>(),
        "SentencePieceOp configuration model is in incorrect format");
    auto str_spm_model = sp_model_ov_any.as<std::string>();
    auto sp_model_const = std::make_shared<Constant>(element::u8, Shape{ str_spm_model.size() }, str_spm_model.data());
    return { sp_model_const };
}




void check_string_input(const Node* node, size_t input_index) {
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+0) == element::i32, "Expected an i32 tensor as the first part of the decomposed string representation");
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+1) == element::i32, "Expected an i32 tensor as the second part of the decomposed string representation");
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+2) == element::u8,  "Expected a u8 tensor as the third part of the decomposed string representation");
}

void check_string_scalar_input(const Node* node, size_t input_index) {
    auto shape = node->get_input_partial_shape(input_index);
    auto element_type = node->get_input_element_type(input_index);

    #if USE_STRING_TENSORS

    OPENVINO_ASSERT(
        (element_type == element::dynamic || element_type == element::string) &&
        (shape.rank().is_dynamic() || shape.rank().get_length() == 0),
        "string/0D tensor is expected");

    #else

    OPENVINO_ASSERT(
        (element_type == element::dynamic || element_type == element::u8) &&
        (shape.rank().is_dynamic() || shape.rank().get_length() == 1),
        "u8/1D tensor is expected");

    #endif
}

void check_ragged_input(const Node* node, size_t input_index) {
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+0) == element::i32, "Expected an i32 tensor as the first part of the decomposed ragged representation");
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+1) == element::i32, "Expected an i32 tensor as the second part of the decomposed ragged representation");
    auto rank = node->get_input_partial_shape(input_index+2).rank();
    FRONT_END_GENERAL_CHECK(rank.is_dynamic() || rank.get_length() == 1, "The last tensor in ragged tensor representation should be a 1D tensor");
}

void check_ragged_string_input(const Node* node, size_t input_index) {
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+0) == element::i32, "Expected an i32 tensor as the first part of the decomposed ragged string representation");
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+1) == element::i32, "Expected an i32 tensor as the second part of the decomposed ragged string representation");
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+2) == element::i32, "Expected an i32 tensor as the third part of the decomposed ragged string representation");
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+3) == element::i32, "Expected an i32 tensor as the forth part of the decomposed ragged string representation");
    FRONT_END_GENERAL_CHECK(node->get_input_element_type(input_index+4) == element::u8,  "Expected a u8 tensor as the fifth part of the decomposed ragged string representation");
}

void set_string_output(Node* node, size_t output_index, const PartialShape& shape) {
    node->set_output_type(output_index+0, element::i32, shape);     // byte offset in output[+2] -- begin of each string
    node->set_output_type(output_index+1, element::i32, shape);     // byte offset in output[+2] -- end of each string
    node->set_output_type(output_index+2, element::u8,  PartialShape{Dimension()});     // symbols from all strings concatenated
}

void set_ragged_string_output(Node* node, size_t output_index, const PartialShape& shape) {
    node->set_output_type(output_index+0, element::i32, shape);     // element offset in output[+2] -- begin of each ragged dimension elements
    node->set_output_type(output_index+1, element::i32, shape);     // element offset in output[+3] -- end of each ragged dimension elements
    node->set_output_type(output_index+2, element::i32, PartialShape{Dimension()}); // byte offset in output[+4] -- begin of each string
    node->set_output_type(output_index+3, element::i32, PartialShape{Dimension()}); // byte offset in output[+4] -- end of each string
    node->set_output_type(output_index+4, element::u8,  PartialShape{Dimension()}); // symbols from all strings cnocatenated
}

void set_ragged_output(Node* node, size_t output_index, const PartialShape& shape, element::Type type) {
    node->set_output_type(output_index+0, element::i32, shape);     // element offset in output[+2] -- begin of each ragged dimension elements
    node->set_output_type(output_index+1, element::i32, shape);     // element offset in output[+2] -- end of each ragged dimension elements
    node->set_output_type(output_index+2, type, PartialShape{Dimension()}); // flatten elements
}


void StringTensorPack::validate_and_infer_types() {
    OPENVINO_ASSERT(m_mode == "begins_ends", "StringTensorPack supports only 'begins_ends' mode, but get " + m_mode);
    check_string_input(this, 0);
    #if USE_STRING_TENSORS
    set_output_type(0, element::string, get_input_partial_shape(0));
    #else
    set_output_type(0, element::u8, PartialShape{Dimension()});
    #endif
}


bool StringTensorPack::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
#if USE_STRING_TENSORS
    // TODO
    return false;
#else
    auto rank = inputs[0].get_shape().size();
    if (rank != 1) {
        std::cerr << "[ WARNING ] StringTensorPack ignores the rank " << rank << " of input tensor and set rank=1 in the output\n";
    }

    auto num_elements = shape_size(inputs[0].get_shape());
    auto num_chars = shape_size(inputs[2].get_shape());
    auto num_output_elements = 4*(1 + 1 + num_elements) + num_chars;
    outputs[0].set_shape(Shape{num_output_elements});

    // FIXME: Do the repacking, otherwise cannot handle string tensors with gaps between strings
    //auto begins = inputs[0].data<const int32_t>();    // this is not needed as no repacking happens in this version of code
    auto ends   = inputs[1].data<const int32_t>();
    auto chars  = inputs[2].data<const uint8_t>();

    auto output = outputs[0].data<uint8_t>();
    auto output_int32 = reinterpret_cast<int32_t*>(output);

    *output_int32++ = num_elements;
    *output_int32++ = 0;
    output_int32 = std::copy(ends, ends + num_elements, output_int32);
    output = reinterpret_cast<uint8_t*>(output_int32);
    output = std::copy(chars, chars + num_chars, output);

    OPENVINO_ASSERT(num_output_elements == output - outputs[0].data<uint8_t>(), "[ INTERNAL ERROR ] StringTensorPack output tensor is corrupted");

    // WARNING! Chars are not repacked. If there are gaps between strings, they will remain.

    return true;
#endif
}



void RaggedTensorPack::validate_and_infer_types() {
    OPENVINO_ASSERT(get_input_size() == 3);
    OPENVINO_ASSERT(get_input_element_type(0) == element::i32);
    OPENVINO_ASSERT(get_input_element_type(1) == element::i32);

    // Pass through the base tensor which is used to build ragged dimensions
    // TODO: Provide correct implementation that saves information about ragged structure
    // TODO: Requires single-tensor packed representation for ragged tensor
    set_output_type(0, get_input_element_type(2), get_input_partial_shape(2));
}


bool RaggedTensorPack::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    // Implementation for debuggin purposes: directly print ragged indices to std::cout and pass the base tensor with elements throug.

    auto input_shape = inputs[0].get_shape();
    //std::cout << "[ DEBUG ] RaggedTensorPack: shape = " << input_shape << "\n";
    auto begins = inputs[0].data<const int32_t>();
    auto ends   = inputs[1].data<const int32_t>();
    auto num_elements = shape_size(input_shape);

    //for(size_t i = 0; i < num_elements; ++i) {
    //std::cout << "[ DEBUG ]     [" << i << "] " << begins[i] << ":" << ends[i] << " with size = " << ends[i] - begins[i] << "\n";
    //}

    inputs[2].copy_to(outputs[0]);

    return true;
}


void StringTensorUnpack::validate_and_infer_types() {
    OPENVINO_ASSERT(
        get_input_size() == 1,
        "Number of inputs for StringTensorUnpack is not equal to 1");

    auto output_shape = PartialShape::dynamic();


    // In case of explicit string tensors the shape is carried by input tensor itself
    // OPENVINO_ASSERT(
    //     input_shape == PartialShape::dynamic(),
    //     "Excplicitly set shape for a string tensor in the unpacking is not supported");

    // There are three cases that affect expected element type of the input tensor:
    // - when string tensor is passed and we are before the hack is applied (element::string) and
    // - when string tensor is passed and we are after the hack in CPU (element::u8) and
    // - when stirng tensor is not really used, and we expect a packed string tensor in this case (element::u8)

    OPENVINO_ASSERT(
#if OPENVINO_ELEMENT_STRING_SUPPORTED
        get_input_element_type(0) == element::string ||
#endif
#if OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK || !USE_STRING_TENSORS
        get_input_element_type(0) == element::u8 ||
#endif
        get_input_element_type(0) == element::dynamic,
        "Type of StringTensorUnpack input is expected to be element::string before a model compilation or element::u8 after the compilation or when element::string is not supported");

#if OPENVINO_ELEMENT_STRING_SUPPORTED
    if(get_input_element_type(0) == element::string) {
        output_shape = get_input_partial_shape(0);
    }
#endif

#if OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK || !USE_STRING_TENSORS
    if(get_input_element_type(0) == element::u8)
    {
        #if OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK
        // After the plugin hack, a tensor is represented as a wrapping u8 tensor that will hold a pointer to a string tensor.
        // The original shape of a string tensor is stored in RT attribute of a tensor descriptor.
        const auto& rt_info = get_input_tensor(0).get_rt_info();
        auto it = rt_info.find("__original_partial_shape");

        // StringTensorUnpack expects __original_partial_shape attribute of type PartialShape in the input tensor.
        // If it is not found that means that model compilation wasn't pass the expected transformation where a string tensor
        // is wrapped to a u8 tensor holding a pointer, or because evaluation of this node is in progress and tensor attributes aren't preserved.
        if(it != rt_info.end() && it->second.is<PartialShape>()) {
            output_shape = it->second.as<PartialShape>();
        } else {
        #endif
            #if !USE_STRING_TENSORS
            // If string tensors shouldn't be used, then the packed u8 format is also expected
            // as an input, but in this case only rank is known
                OPENVINO_ASSERT(
                    get_input_partial_shape(0).rank().is_dynamic() || get_input_partial_shape(0).rank().get_length() == 1,
                    "StringTensorUnpack expects a u8 tensor with rank 1 that holds packed batched string tensor as an input, but observes type " +
                        get_input_element_type(0).get_type_name() + " and shape " + get_input_partial_shape(0).to_string());

            output_shape = PartialShape({Dimension()});  // [?]
            #endif
        #if OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK
        }
        #endif
    }
#endif

    OPENVINO_ASSERT(m_mode == "begins_ends", "StringTensorUnpack supporst only 'begins_ends' mode, but get " + m_mode);

    if (m_mode == "begins_ends") {
        set_string_output(this, 0, output_shape);
    }
}

void unpack_strings (const std::string* strings, const Shape shape, ov::Tensor& begins, ov::Tensor& ends, ov::Tensor& chars) { // TODO: no need for a reference to a ov::Tensor?
    auto nelements = shape_size(shape);

    size_t total = 0;
    for(size_t i = 0; i < nelements; ++i)
        total += strings[i].length();

    begins.set_shape(shape);
    ends.set_shape(shape);
    chars.set_shape(Shape{total});

    auto pbegins = begins.data<int32_t>();
    auto pends = ends.data<int32_t>();
    auto poutput_symbols = reinterpret_cast<char*>(chars.data<uint8_t>());
    size_t offset = 0;

    for(size_t i = 0; i < nelements; ++i)
    {
        pbegins[i] = offset;
        poutput_symbols = std::copy(strings[i].begin(), strings[i].end(), poutput_symbols);
        offset += strings[i].length();
        pends[i] = offset;
    }
}

bool StringTensorUnpack::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    auto ptensor = &inputs[0];
    #if OPENVINO_USE_INPUT_OUTPUT_STRING_TENSOR_HACK
    if(ptensor->get_element_type() == element::u8 && ptensor->get_byte_size() == sizeof(void*)) {
        auto data = *reinterpret_cast<const void* const*>(ptensor->data());
        if(data != nullptr) {
            ptensor = reinterpret_cast<const ov::Tensor*>(data);
        }
    }
    #endif

    auto tensor = *ptensor;

#if OPENVINO_ELEMENT_STRING_SUPPORTED
    if(tensor.get_element_type() == element::string) {
        Shape input_shape = tensor.get_shape();
        const std::string* input_strings = tensor.data<std::string>();
        unpack_strings(input_strings, input_shape, outputs[0], outputs[1], outputs[2]);
        return true;
    } else {
#endif

#if USE_STRING_TENSORS
    OPENVINO_ASSERT(false, "Detected a u8 tensor but element::string tensor should be provided")
#endif

    int32_t batch_size;
    const int32_t* begin_ids;
    const int32_t* end_ids;
    const uint8_t* data;
    parse_packed_strings(tensor, batch_size, begin_ids, end_ids, data);
    auto num_chars = end_ids[batch_size - 1];

    outputs[0].set_shape(Shape{static_cast<unsigned long>(batch_size)});
    outputs[1].set_shape(Shape{static_cast<unsigned long>(batch_size)});
    outputs[2].set_shape(Shape{static_cast<unsigned long>(num_chars)});
    auto begins = outputs[0].data<int32_t>();
    auto ends = outputs[1].data<int32_t>();
    auto chars = outputs[2].data<uint8_t>();
    std::copy(begin_ids, begin_ids + batch_size, begins);
    std::copy(end_ids, end_ids + batch_size, ends);
    std::copy(data, data + num_chars, chars);

    return true;

#if OPENVINO_ELEMENT_STRING_SUPPORTED
    }
#endif
}


void override_parameter (std::shared_ptr<ov::Node> node, element::Type type, const PartialShape& shape) {
    if (auto parameter = std::dynamic_pointer_cast<Parameter>(node)) {
        // TODO: Apply this change conditionally based on real Parameter value
        std::cerr << "Overriding Parameter element_type to " << type << " and shape " << shape << "\n";
        parameter->set_partial_shape(shape);
        parameter->set_element_type(type);
        parameter->validate_and_infer_types();
    }
}

// TODO: replace NodeContext and input_index by a single input
OutputVector pre_translate_string_tensor_input(ov::Output<ov::Node> input) {
    auto input_node = input.get_node_shared_ptr();

#if !USE_STRING_TENSORS
    override_parameter(input_node, element::u8, PartialShape{Dimension()});
#endif

    if (auto struct_pack = std::dynamic_pointer_cast<StringTensorPack>(input_node)) {
        FRONT_END_GENERAL_CHECK(struct_pack->get_input_size() == 3, "Expected 3 inputs to StringTensorPack which represents a string tensor");
        return struct_pack->input_values();
    } else {
        #if USE_STRING_TENSORS || true     // always
        return std::make_shared<StringTensorUnpack>(OutputVector{input}, "begins_ends")->outputs();
        #else
        // Suppose this is u8 packed string tensor with a single batch dimension
        // Unpack this tensor using standard operations

        // Cannot do that because there is not ReinterprectCast operation in OV
        // TODO: Find a way to make it without reinterpretation operation or introduce it as an extension (easy)
        #endif
    }
}



OutputVector pre_translate_ragged_tensor_input(ov::Output<ov::Node> input) {
    auto ragged_pack = dynamic_cast<RaggedTensorPack*>(input.get_node());
    OPENVINO_ASSERT(ragged_pack, "Expected RaggedTensorPack but didn't find it");
    return ragged_pack->input_values();
}

OutputVector pre_translate_ragged_string_tensor_input(ov::Output<ov::Node> input) {
    // auto ragged_pack = dynamic_cast<RaggedTensorPack*>(node.get_input(input_index).get_node());
    // OPENVINO_ASSERT(ragged_pack, "Expected RaggedTensorPack but didn't find it");
    auto ragged_inputs = pre_translate_ragged_tensor_input(input);
    auto string_inputs = pre_translate_string_tensor_input(ragged_inputs[2]);
    ragged_inputs.pop_back();
    ragged_inputs.insert(ragged_inputs.end(), string_inputs.begin(), string_inputs.end());
    // auto string_pack = dynamic_cast<StringTensorPack*>(ragged_pack->get_input_node_ptr(2));
    // OPENVINO_ASSERT(string_pack, "Expected StringTensorPack as a base for RaggedTensorPack but didn't find it");
    return ragged_inputs;
}

ov::Output<ov::Node> post_translate_string_tensor_output(const OutputVector& outputs) {
    FRONT_END_GENERAL_CHECK(outputs.size() == 3, "Expected 3 tensors in decomposed string tensor representation");
    return std::make_shared<StringTensorPack>(outputs, "begins_ends");
}

ov::Output<ov::Node> post_translate_ragged_tensor_output(const OutputVector& outputs) {
    FRONT_END_GENERAL_CHECK(outputs.size() == 3, "Expected 3 tensors in decomposed string tensor representation");
    return std::make_shared<RaggedTensorPack>(outputs);
}

NamedOutputVector translate_sentencepiece_tokenizer(const NodeContext& node) {
    // this is custom translator that converts a sub-graph with SentencePieceOp, SentencePieceTokenizer,
    // and RaggedTensorToSparse operation- into a custom operation SentencepieceTokenizerExtensionOp
    FRONT_END_GENERAL_CHECK(node.get_input_size() > 0, "RaggedTensorToSparse expects at least one input.");
    auto node_name = node.get_name();

    // check that producers of RaggedTensorToSparse is SentencePieceTokenizer
    auto sp_tokenize_op = node.get_input(0).get_node_shared_ptr();
    FRONT_END_GENERAL_CHECK(sp_tokenize_op->get_input_size() > 6,
        "SentencepieceTokenizeOp expects at least six inputs");

    // prepare inputs that go to custom operation
    // prepare input 0 - SentencePieceTokenizer configuration model
    auto sp_model_const = as_type_ptr<Constant>(sp_tokenize_op->input_value(0).get_node_shared_ptr());
    FRONT_END_GENERAL_CHECK(sp_model_const, "Conversion expects SentencePiece model to be constant.");

    // prepare input six inputs
    auto inputs = sp_tokenize_op->input_value(1);

    // extract values for nbest_size, alpha, add_bos, add_eos, reverse attributes
    auto nbest_size = extract_scalar_const_value<int32_t>(sp_tokenize_op->input_value(2).get_node_shared_ptr(), "nbest_size");
    auto alpha = extract_scalar_const_value<float>(sp_tokenize_op->input_value(3).get_node_shared_ptr(), "alpha");
    auto add_bos = extract_scalar_const_value<bool>(sp_tokenize_op->input_value(4).get_node_shared_ptr(), "add_bos");
    auto add_eos = extract_scalar_const_value<bool>(sp_tokenize_op->input_value(5).get_node_shared_ptr(), "add_eos");
    auto reverse = extract_scalar_const_value<bool>(sp_tokenize_op->input_value(6).get_node_shared_ptr(), "reverse");

#if !USE_STRING_TENSORS
    // Override type of input tensor if this is a Parameter
    if (auto parameter = std::dynamic_pointer_cast<Parameter>(inputs.get_node_shared_ptr())) {
        parameter->set_partial_shape(PartialShape{ Dimension() });
        parameter->set_element_type(element::u8);
        parameter->validate_and_infer_types();
    }
#endif

#if SENTENCE_PIECE_EXTENSION_DECOMPOSED_STRINGS

    OutputVector inputs_vector = OutputVector{ sp_model_const };
    auto unpacked_outputs = std::make_shared<StringTensorUnpack>(OutputVector{inputs}, "begins_ends")->outputs();
    inputs_vector.insert(inputs_vector.end(), unpacked_outputs.begin(), unpacked_outputs.end());

#else

    OutputVector inputs_vector = OutputVector{ sp_model_const, inputs };

#endif

    // create a node with custom operation
    auto sp_tokenizer_ext = std::make_shared<SentencepieceTokenizer>(inputs_vector, nbest_size, alpha, add_bos, add_eos, reverse);
    FRONT_END_GENERAL_CHECK(sp_tokenizer_ext->get_output_size() == 3,
        "Internal error: SentencepieceTokenizer operation extension must have three outputs.");

    // set tensor names
    sp_tokenizer_ext->output(0).add_names({ node_name + ":0" });
    sp_tokenizer_ext->output(1).add_names({ node_name + ":1" });
    sp_tokenizer_ext->output(2).add_names({ node_name + ":2" });

    // create named outputs for the conversion extension
    NamedOutputVector named_results;
    named_results.push_back({ "sparse_indices", sp_tokenizer_ext->output(0) });
    named_results.push_back({ "sparse_values", sp_tokenizer_ext->output(1) });
    named_results.push_back({ "sparse_dense_shape", sp_tokenizer_ext->output(2) });

    return named_results;
}

bool evaluate_normalization_helper (ov::TensorVector& outputs, const ov::TensorVector& inputs, std::function<std::string(const std::string&)> normalizer) {
    auto begins = inputs[0].data<const int32_t>();
    auto ends   = inputs[1].data<const int32_t>();
    auto chars  = inputs[2].data<const uint8_t>();

    // Set output shapes
    outputs[0].set_shape(inputs[0].get_shape());
    outputs[1].set_shape(inputs[1].get_shape());
    const size_t num_elements = inputs[0].get_size();

    // TODO: How to avoid copying from this temporary buffer?
    // TODO: It can be possible to collect output symbols directly in the output tensor memory if `normalizer` has reasonable estimation for the final size.
    std::deque<uint8_t> buffer;

    // For the whole implementation below the input shapes can be ignored, we are working with the flatten representaions
    // and only number of elements in the original tensors matter

    // Get pointers in the output tensors
    auto new_begins = outputs[0].data<int32_t>();
    auto new_ends   = outputs[1].data<int32_t>();

    for(size_t i = 0; i < num_elements; ++i) {
        new_begins[i] = buffer.size();
        std::string new_str = normalizer(std::string(chars + begins[i], chars + ends[i]));
        buffer.insert(buffer.end(), new_str.begin(), new_str.end());
        new_ends[i] = buffer.size();
    }

    // Copy collected symbols to the target output tensor

    outputs[2].set_shape(Shape{buffer.size()});
    auto new_chars  = outputs[2].data<uint8_t>();
    std::copy(buffer.begin(), buffer.end(), new_chars);

    return true;
}


void CaseFold::validate_and_infer_types() {
    check_string_input(this, 0);
    set_string_output(this, 0, get_input_partial_shape(0));
}

bool CaseFold::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
#if 1

    return evaluate_normalization_helper(
        outputs, inputs,
        [](const std::string& str) {
            using namespace paddlenlp::fast_tokenizer;
            return normalizers::NormalizedString(str).Lowercase().GetStr();
        });

#else
    // Stub implementation that transforms each input string "X" to "CaseFold(X)" for debugging purposes
    {
        auto begins = inputs[0].data<const int32_t>();
        auto ends   = inputs[1].data<const int32_t>();
        auto chars  = inputs[2].data<const uint8_t>();

        // Set output shapes
        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());
        const std::string left_side = "CaseFold(", right_side = ")";
        const size_t num_elements = inputs[0].get_size();
        const size_t new_len = inputs[2].get_size() + (left_side.length() + right_side.length())*num_elements;
        outputs[2].set_shape(Shape{new_len});

        // For the whole implementation below the input shapes can be ignored, we are working with the flatten representaions
        // and only number of elements in the original tensors matter

        // Get pointers in the output tensors
        auto new_begins = outputs[0].data<int32_t>();
        auto new_ends   = outputs[1].data<int32_t>();
        auto new_chars  = outputs[2].data<uint8_t>();
        int32_t char_offset = 0;

        for(size_t i = 0; i < num_elements; ++i) {
            new_begins[i] = char_offset;
            std::string new_str = left_side + std::string(chars + begins[i], chars + ends[i]) + right_side;
            std::copy(new_str.data(), new_str.data() + new_str.length(), new_chars + char_offset);
            char_offset += new_str.length();
            new_ends[i] = char_offset;
        }
        return true;
    }
    // End of stub implementation
#endif
}


ov::OutputVector translate_case_fold_utf8(const ov::frontend::NodeContext& node) {
    FRONT_END_GENERAL_CHECK(node.get_input_size() == 1, "CaseFold expects only 1 input");
    return { post_translate_string_tensor_output(std::make_shared<CaseFold>(
        pre_translate_string_tensor_input(node.get_input(0)))->outputs()) };
}

namespace {
using namespace paddlenlp::fast_tokenizer::normalizers;
using NormalizersMap = std::map<std::string, std::function<std::string(const std::string&)>>;

const NormalizersMap normalizers = {
    {"NFD", [](const std::string& str) { return NormalizedString(str).NFD().GetStr(); }},
    {"NFC", [](const std::string& str) { return NormalizedString(str).NFC().GetStr(); }},
    {"NFKD", [](const std::string& str) { return NormalizedString(str).NFKD().GetStr(); }},
    {"NFKC", [](const std::string& str) { return NormalizedString(str).NFKC().GetStr(); }},
};

}


void NormalizeUnicode::validate_and_infer_types() {
    check_string_input(this, 0);
    OPENVINO_ASSERT(normalizers.find(m_normalization_form) != normalizers.end(), "NormalizeUnicode doesn't know normalization form " + m_normalization_form);
    set_string_output(this, 0, get_input_partial_shape(0));
}

bool NormalizeUnicode::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
#if 1

    return evaluate_normalization_helper(outputs, inputs, normalizers.at(m_normalization_form));

#else

    auto begins = inputs[0].data<const int32_t>();
    auto ends   = inputs[1].data<const int32_t>();
    auto chars  = inputs[2].data<const uint8_t>();

    // Stub implementation that transforms each input string "X" to "NormalizeUnicode(X, normalization_form)" for debugging purposes
    {
        // Set output shapes
        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());
        const std::string left_side = "NormalizeUnicode(", right_side = ")", delimeter = ", ";
        const size_t num_elements = inputs[0].get_size();
        const size_t new_len = inputs[2].get_size() + (left_side.length() + right_side.length() + delimeter.length() + m_normalization_form.length())*num_elements;
        outputs[2].set_shape(Shape{new_len});

        // For the whole implementation below the input shapes can be ignored, we are working with the flatten representaions
        // and only number of elements in the original tensors matter

        // Get pointers in the output tensors
        auto new_begins = outputs[0].data<int32_t>();
        auto new_ends   = outputs[1].data<int32_t>();
        auto new_chars  = outputs[2].data<uint8_t>();
        int32_t char_offset = 0;

        for(size_t i = 0; i < num_elements; ++i) {
            new_begins[i] = char_offset;
            std::string new_str = left_side + std::string(chars + begins[i], chars + ends[i]) + delimeter + m_normalization_form + right_side;
            std::copy(new_str.data(), new_str.data() + new_str.length(), new_chars + char_offset);
            char_offset += new_str.length();
            new_ends[i] = char_offset;
        }
        return true;
    }
    // End of stub implementation
#endif
}


ov::OutputVector translate_normalize_utf8(const ov::frontend::NodeContext& node) {
    FRONT_END_GENERAL_CHECK(node.get_input_size() == 1, "NormalizeUTF8 expects only 1 input");
    return { post_translate_string_tensor_output(std::make_shared<NormalizeUnicode>(
        pre_translate_string_tensor_input(node.get_input(0)),
        node.get_attribute<std::string>("normalization_form"))->outputs()) };
}


void RegexNormalization::validate_and_infer_types() {
    check_string_input(this, 0);
    check_string_scalar_input(this, 3);
    check_string_scalar_input(this, 4);
    set_string_output(this, 0, get_input_partial_shape(0));
}

bool RegexNormalization::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    auto search_pattern_buf  = inputs[3].data<const uint8_t>();
    auto replace_pattern_buf  = inputs[4].data<const uint8_t>();
    auto search_pattern = absl::string_view((const char*)search_pattern_buf, shape_size(inputs[3].get_shape()) - 1);   // FIXME: -1 is a complementary change to a WA applied in string_attribute_to_constant
    auto replace_pattern = absl::string_view((const char*)replace_pattern_buf, shape_size(inputs[4].get_shape()) - 1);   // FIXME: -1 is a complementary change to a WA applied in string_attribute_to_constant

#if 1

    using namespace paddlenlp::fast_tokenizer::normalizers;
    re2::RE2 search_pattern_re(search_pattern);

    return evaluate_normalization_helper(
        outputs, inputs,
        [&replace_pattern, &search_pattern_re](const std::string& str) {
            return NormalizedString(str).Replace(search_pattern_re, std::string(replace_pattern)).GetStr();
    });

#else
    // Stub implementation that transforms each input string "X" to "RegexNormalization(X, search_pattern, replace_pattern)" for debugging purposes
    {
        auto begins = inputs[0].data<const int32_t>();
        auto ends   = inputs[1].data<const int32_t>();
        auto chars  = inputs[2].data<const uint8_t>();

        // Set output shapes
        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());
        const std::string left_side = "RegexNormalization(", right_side = ")", delimeter = ", ";
        const size_t num_elements = inputs[0].get_size();
        const size_t new_len = inputs[2].get_size() + (left_side.length() + right_side.length() + 2*delimeter.length() + search_pattern.length() + replace_pattern.length())*num_elements;
        outputs[2].set_shape(Shape{new_len});

        // For the whole implementation below the input shapes can be ignored, we are working with the flatten representaions
        // and only number of elements in the original tensors matter

        // Get pointers in the output tensors
        auto new_begins = outputs[0].data<int32_t>();
        auto new_ends   = outputs[1].data<int32_t>();
        auto new_chars  = outputs[2].data<uint8_t>();
        int32_t char_offset = 0;

        for(size_t i = 0; i < num_elements; ++i) {
            new_begins[i] = char_offset;

            std::string new_str =
                left_side + std::string(chars + begins[i], chars + ends[i]) + delimeter +
                std::string(search_pattern) + delimeter +
                std::string(replace_pattern) + right_side;

            std::copy(new_str.data(), new_str.data() + new_str.length(), new_chars + char_offset);
            char_offset += new_str.length();
            new_ends[i] = char_offset;
        }
        return true;
    }
    // End of stub implementation
#endif
}


std::shared_ptr<Node> string_attribute_to_constant (const ov::frontend::NodeContext& node, const std::string& name) {
    // FIXME: using space to pad the value to work-around CPU issue with empty constants
    auto value = node.get_attribute<std::string>(name) + " ";

    // TODO: How to translate attribute `replace_global`?

    #if USE_STRING_TENSORS
    return std::make_shared<Constant>(element::string, {}, value);
    #else
    return std::make_shared<Constant>(element::u8, Shape{value.length()}, (const void*)value.data());
    #endif
}


ov::OutputVector translate_static_regex_replace(const ov::frontend::NodeContext& node) {
    FRONT_END_GENERAL_CHECK(node.get_input_size() == 1, "StaticRegexReplace expects only 1 input");
    ov::OutputVector inputs = pre_translate_string_tensor_input(node.get_input(0));
    inputs.push_back(string_attribute_to_constant(node, "pattern"));
    inputs.push_back(string_attribute_to_constant(node, "rewrite"));
    return { post_translate_string_tensor_output(std::make_shared<RegexNormalization>(inputs)->outputs()) };
}


namespace {

using paddlenlp::fast_tokenizer::core::SplitMode;
const std::map<std::string, SplitMode> split_modes = {
    {"remove", SplitMode::REMOVED},
    {"isolate", SplitMode::ISOLATED},
    {"merge_with_previous", SplitMode::MERGED_WITH_PREVIOUS},
    {"merge_with_next", SplitMode::MERGED_WITH_NEXT},
};

}


void RegexSplit::validate_and_infer_types() {
//    check_string_input(this, 0);
//    check_string_scalar_input(this, 3);
//    check_ragged_string_input(this, 0);
//    check_string_input(this, 5);
    OPENVINO_ASSERT(split_modes.find(m_behaviour) != split_modes.end(), "RegexSplit doesn't support unknown split mode: " + m_behaviour);
    set_ragged_string_output(this, 0, get_input_partial_shape(0));
}

bool RegexSplit::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {

    if (inputs.size() < 5) {
        auto begins = inputs[0].data<const int32_t>();
        auto ends   = inputs[1].data<const int32_t>();
        auto chars  = inputs[2].data<const uint8_t>();

        ov::Tensor ragged_begins_tensor(ov::element::i32, inputs[0].get_shape());
        ov::Tensor ragged_ends_tensor(ov::element::i32, inputs[0].get_shape());
        auto ragged_begins = ragged_begins_tensor.data<int32_t>();
        auto ragged_ends = ragged_ends_tensor.data<int32_t>();
        for (int i=0; i < inputs[0].get_size(); ++i) {
            ragged_begins[i] = i;
            ragged_ends[i] = i + 1;
        };

        auto split_pattern_buf  = inputs[3].data<const uint8_t>();
        auto split_pattern = absl::string_view((const char*)split_pattern_buf, shape_size(inputs[3].get_shape())/* - 1*/);   // Shouldn't be applied FIXME: -1 is a complementary change to a WA applied in string_attribute_to_constant

        const size_t num_elements = inputs[0].get_size();
        const size_t num_chars = inputs[2].get_size();

        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());

        outputs[2].set_shape(Shape{num_chars});
        outputs[3].set_shape(Shape{num_chars});

        outputs[4] = inputs[2];  // TODO: Does it really work?

        // For the whole implementation below the input shapes can be ignored, we are working with the flatten representaions
        // and only number of elements in the original tensors matter

        // Get pointers in the output tensors
        auto new_ragged_begins = outputs[0].data<int32_t>();
        auto new_ragged_ends   = outputs[1].data<int32_t>();
        auto new_begins = outputs[2].data<int32_t>();
        auto new_ends   = outputs[3].data<int32_t>();
        int32_t ragged_offset = 0;

        using namespace paddlenlp::fast_tokenizer;
        auto pretokenizer = pretokenizers::SplitPreTokenizer(std::string(split_pattern), split_modes.at(m_behaviour), m_invert);

        std::cerr << "[ RegexSplit ] regex: " << std::string(split_pattern) << "\n";

        for(size_t seq = 0; seq < num_elements; ++seq) {
            for(size_t word = ragged_begins[seq]; word < ragged_ends[seq]; ++word) {

                auto str = std::string(chars + begins[word], chars + ends[word]);
                std::cerr << "[ RegexSplit ] old_str: " << str << "\n";
                paddlenlp::fast_tokenizer::pretokenizers::PreTokenizedString pretokenized(str);
                pretokenizer(&pretokenized);
                size_t num_splits = pretokenized.GetSplitsSize();

                new_ragged_begins[seq] = ragged_offset;

                for (size_t j = 0; j < num_splits; ++j) {
                    auto split = pretokenized.GetSplit(j);
                    const auto& value = split.normalized_.GetStr();
                    auto offset = split.normalized_.GetOrginalOffset();
                    std::cerr << "[ RegexSplit ]     split part: '" << value << "'\n";
                    std::cerr << "[ RegexSplit ]     split offs: " << offset.first << ":" << offset.second << "\n";
                    new_begins[ragged_offset] = begins[word] + offset.first;
                    new_ends[ragged_offset] = begins[word] + offset.second;

                    ++ragged_offset;
                };
            }

            new_ragged_ends[seq] = ragged_offset;
        }

        // Fix real shape based on collected results
        outputs[2].set_shape({ragged_offset});
        outputs[3].set_shape({ragged_offset});
    } else {
        auto ragged_begins = inputs[0].data<const int32_t>();
        auto ragged_ends   = inputs[1].data<const int32_t>();
        auto begins = inputs[2].data<const int32_t>();
        auto ends   = inputs[3].data<const int32_t>();
        auto chars  = inputs[4].data<const uint8_t>();

        auto split_pattern_buf  = inputs[5].data<const uint8_t>();
        auto split_pattern = absl::string_view((const char*)split_pattern_buf, shape_size(inputs[5].get_shape())/* - 1*/);   // Shouldn't be applied FIXME: -1 is a complementary change to a WA applied in string_attribute_to_constant

        outputs[4] = inputs[4];
        const size_t num_elements = inputs[2].get_size();
        const size_t num_chars = inputs[4].get_size();

        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());

        outputs[2].set_shape(Shape{num_chars});
        outputs[3].set_shape(Shape{num_chars});

        outputs[4] = inputs[4];  // TODO: Does it really work?

        // For the whole implementation below the input shapes can be ignored, we are working with the flatten representaions
        // and only number of elements in the original tensors matter

        // Get pointers in the output tensors
        auto new_ragged_begins = outputs[0].data<int32_t>();
        auto new_ragged_ends   = outputs[1].data<int32_t>();
        auto new_begins = outputs[2].data<int32_t>();
        auto new_ends   = outputs[3].data<int32_t>();
        int32_t ragged_offset = 0;

        using namespace paddlenlp::fast_tokenizer;
        auto pretokenizer = pretokenizers::SplitPreTokenizer(std::string(split_pattern), split_modes.at(m_behaviour), m_invert);

        for(size_t seq = 0; seq < num_elements; ++seq) {
            for(size_t word = ragged_begins[seq]; word < ragged_ends[seq]; ++word) {

                auto str = std::string(chars + begins[word], chars + ends[word]);
                std::cerr << "[ RegexSplit ] old_str: " << str << "\n";
                paddlenlp::fast_tokenizer::pretokenizers::PreTokenizedString pretokenized(str);
                pretokenizer(&pretokenized);
                size_t num_splits = pretokenized.GetSplitsSize();

                new_ragged_begins[seq] = ragged_offset;

                for (size_t j = 0; j < num_splits; ++j) {
                    auto split = pretokenized.GetSplit(j);
                    const auto& value = split.normalized_.GetStr();
                    auto offset = split.normalized_.GetOrginalOffset();
                    std::cerr << "[ RegexSplit ]     split part: " << value << "\n";
                    std::cerr << "[ RegexSplit ]     split offs: " << offset.first << ":" << offset.second << "\n";
                    new_begins[ragged_offset] = begins[word] + offset.first;
                    new_ends[ragged_offset] = begins[word] + offset.second;

                    ++ragged_offset;
                };
            }

            new_ragged_ends[seq] = ragged_offset;
        }

        // Fix real shape based on collected results
        outputs[2].set_shape({ragged_offset});
        outputs[3].set_shape({ragged_offset});
    }
#if 1

    // Set output shapes
//    outputs[0].set_shape(inputs[0].get_shape());
//    outputs[1].set_shape(inputs[1].get_shape());
//
//    const size_t num_elements = inputs[0].get_size();
//    const size_t num_chars = inputs[2].get_size();

    // TODO: Better estimations for max size?
    // Assume we cannot have empty parts, so the number of parts cannot be bigger than the number of symbols
//    outputs[2].set_shape(Shape{num_chars});
//    outputs[3].set_shape(Shape{num_chars});

    // Assume we cannot introduce new symbols to output, only existing can be distributed (with gaps)

    // TODO: Can we just route input tensor directly to the output outside evaluate when graph is being constructed?
//    outputs[4] = inputs[2];  // TODO: Does it really work?

    // If line above doesn't work, do this instead:
    //outputs[4].set_shape(Shape{num_chars});
    //inputs[2].copy_to(outputs[4]);

    return true;

    // TODO: Complete implementation
#else
    // Stub implementation that transforms each input string "X" to multiple "RegexSplit(X, split_pattern) = part(X)" for debugging purposes
    // Where part(X) is a part of original X divided by predefined length with some reminder
    // So each element X is divided into multiple output elements along ragged dimension, and the number of elements depends on the input X length and
    // can vary for different X. For example, let the length = 2 and input X = "words", the output would consist of 3 elements along corresponding
    // ragged dimension in the output with values:
    //  - "RegexSplit(word, search_pattern, replace_pattern) = wo",
    //  - "RegexSplit(word, search_pattern, replace_pattern) = rd",
    //  - "RegexSplit(word, search_pattern, replace_pattern) = s"
    // split_pattern is cut for the sake of readability of ouput
    {
        const size_t part_length = 30;   // any positive number, defines the length of each part in bytes

        std::string split_pattern_part = std::string(split_pattern.substr(0, part_length));

        // Set output shapes
        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());

        const std::string left_side = "RegexSplit(", right_side = ")", delimeter = ", ";
        const size_t num_elements = inputs[0].get_size();
        size_t num_parts = 0;   // will count the number of all parts
        size_t num_additional_chars = 0;  //
        // Count the resulting number of part that we are going to obtain
        for(size_t i = 0; i < num_elements; ++i) {
            auto length = ends[i] - begins[i];
            auto num_of_whole_parts = length/part_length;
            auto remainder = length%part_length;
            auto num_local_parts = num_of_whole_parts + int(bool(remainder));
            num_parts += num_local_parts;
            num_additional_chars += length*num_local_parts;
        }

        size_t num_chars = inputs[2].get_size();

        // FIXME: Overestimation
        const size_t new_num_chars = num_chars + num_parts*30/*!*/ + (left_side.length() + right_side.length() + delimeter.length() + split_pattern_part.length())*num_elements;
        outputs[2].set_shape(Shape{num_parts});
        outputs[3].set_shape(Shape{num_parts});
        outputs[4].set_shape(Shape{new_num_chars});

        // For the whole implementation below the input shapes can be ignored, we are working with the flatten representaions
        // and only number of elements in the original tensors matter

        // Get pointers in the output tensors
        auto new_ragged_begins = outputs[0].data<int32_t>();
        auto new_ragged_ends   = outputs[1].data<int32_t>();
        auto new_begins = outputs[2].data<int32_t>();
        auto new_ends   = outputs[3].data<int32_t>();
        auto new_chars  = outputs[4].data<uint8_t>();
        int32_t ragged_offset = 0;
        int32_t char_offset = 0;

        for(size_t i = 0; i < num_elements; ++i) {
            new_ragged_begins[i] = ragged_offset;
            auto old_str = std::string(chars + begins[i], chars + ends[i]);
            auto new_str_part_base = left_side + old_str + delimeter + split_pattern_part + right_side;

            for(size_t j = 0; j < old_str.length(); j += part_length) {
                new_begins[ragged_offset] = char_offset;
                //auto new_str_part = new_str_part_base + old_str.substr(j, part_length);
                std::string new_str_part = j == 0 ? new_str_part_base : "part[" + std::to_string(i) + "," + std::to_string(j) + "]";
                std::copy(new_str_part.data(), new_str_part.data() + new_str_part.length(), new_chars + char_offset);
                char_offset += new_str_part.length();
                new_ends[ragged_offset] = char_offset;
                ++ragged_offset;
            }

            new_ragged_ends[i] = ragged_offset;
        }

        outputs[4].set_shape({char_offset});

        //OPENVINO_ASSERT(char_offset == new_num_chars, "Internal error in RegexSplit::evaluate: out of range for chars");
        OPENVINO_ASSERT(ragged_offset == num_parts, "Internal error in RegexSplit::evaluate: out of range for ragged parts");

        return true;
    }
    // End of stub implementation
#endif
}


ov::OutputVector translate_regex_split_with_offsets(const ov::frontend::NodeContext& node) {
    FRONT_END_GENERAL_CHECK(node.get_input_size() == 3, "RegexSplitWithOffsets expects 3 inputs");
    ov::OutputVector inputs = pre_translate_string_tensor_input(node.get_input(0));
    auto delim_regex_pattern = node.get_input(1).get_node()->input_value(2);    // use u8 part of packed string tensor as we are expecting a scalar string: TODO: verify it is really there
    inputs.push_back(delim_regex_pattern);
    // TODO: Use node.get_input(2) with keep_delim_regex_pattern, most likely it should be handled in another RegexSplit with `isolated` behaviour
    auto outputs = std::make_shared<RegexSplit>(inputs)->outputs();
    auto flatten_string_tensor = post_translate_string_tensor_output({outputs[2], outputs[3], outputs[4]});
    return { post_translate_ragged_tensor_output({outputs[0], outputs[1], flatten_string_tensor}) };
}


const std::unordered_map<uint8_t, std::vector<uint8_t>> create_bytes_to_chars_map() {
    return {
        { 33 , { 33 }},
        { 34 , { 34 }},
        { 35 , { 35 }},
        { 36 , { 36 }},
        { 37 , { 37 }},
        { 38 , { 38 }},
        { 39 , { 39 }},
        { 40 , { 40 }},
        { 41 , { 41 }},
        { 42 , { 42 }},
        { 43 , { 43 }},
        { 44 , { 44 }},
        { 45 , { 45 }},
        { 46 , { 46 }},
        { 47 , { 47 }},
        { 48 , { 48 }},
        { 49 , { 49 }},
        { 50 , { 50 }},
        { 51 , { 51 }},
        { 52 , { 52 }},
        { 53 , { 53 }},
        { 54 , { 54 }},
        { 55 , { 55 }},
        { 56 , { 56 }},
        { 57 , { 57 }},
        { 58 , { 58 }},
        { 59 , { 59 }},
        { 60 , { 60 }},
        { 61 , { 61 }},
        { 62 , { 62 }},
        { 63 , { 63 }},
        { 64 , { 64 }},
        { 65 , { 65 }},
        { 66 , { 66 }},
        { 67 , { 67 }},
        { 68 , { 68 }},
        { 69 , { 69 }},
        { 70 , { 70 }},
        { 71 , { 71 }},
        { 72 , { 72 }},
        { 73 , { 73 }},
        { 74 , { 74 }},
        { 75 , { 75 }},
        { 76 , { 76 }},
        { 77 , { 77 }},
        { 78 , { 78 }},
        { 79 , { 79 }},
        { 80 , { 80 }},
        { 81 , { 81 }},
        { 82 , { 82 }},
        { 83 , { 83 }},
        { 84 , { 84 }},
        { 85 , { 85 }},
        { 86 , { 86 }},
        { 87 , { 87 }},
        { 88 , { 88 }},
        { 89 , { 89 }},
        { 90 , { 90 }},
        { 91 , { 91 }},
        { 92 , { 92 }},
        { 93 , { 93 }},
        { 94 , { 94 }},
        { 95 , { 95 }},
        { 96 , { 96 }},
        { 97 , { 97 }},
        { 98 , { 98 }},
        { 99 , { 99 }},
        { 100 , { 100 }},
        { 101 , { 101 }},
        { 102 , { 102 }},
        { 103 , { 103 }},
        { 104 , { 104 }},
        { 105 , { 105 }},
        { 106 , { 106 }},
        { 107 , { 107 }},
        { 108 , { 108 }},
        { 109 , { 109 }},
        { 110 , { 110 }},
        { 111 , { 111 }},
        { 112 , { 112 }},
        { 113 , { 113 }},
        { 114 , { 114 }},
        { 115 , { 115 }},
        { 116 , { 116 }},
        { 117 , { 117 }},
        { 118 , { 118 }},
        { 119 , { 119 }},
        { 120 , { 120 }},
        { 121 , { 121 }},
        { 122 , { 122 }},
        { 123 , { 123 }},
        { 124 , { 124 }},
        { 125 , { 125 }},
        { 126 , { 126 }},
        { 161 , { 194, 161 }},
        { 162 , { 194, 162 }},
        { 163 , { 194, 163 }},
        { 164 , { 194, 164 }},
        { 165 , { 194, 165 }},
        { 166 , { 194, 166 }},
        { 167 , { 194, 167 }},
        { 168 , { 194, 168 }},
        { 169 , { 194, 169 }},
        { 170 , { 194, 170 }},
        { 171 , { 194, 171 }},
        { 172 , { 194, 172 }},
        { 174 , { 194, 174 }},
        { 175 , { 194, 175 }},
        { 176 , { 194, 176 }},
        { 177 , { 194, 177 }},
        { 178 , { 194, 178 }},
        { 179 , { 194, 179 }},
        { 180 , { 194, 180 }},
        { 181 , { 194, 181 }},
        { 182 , { 194, 182 }},
        { 183 , { 194, 183 }},
        { 184 , { 194, 184 }},
        { 185 , { 194, 185 }},
        { 186 , { 194, 186 }},
        { 187 , { 194, 187 }},
        { 188 , { 194, 188 }},
        { 189 , { 194, 189 }},
        { 190 , { 194, 190 }},
        { 191 , { 194, 191 }},
        { 192 , { 195, 128 }},
        { 193 , { 195, 129 }},
        { 194 , { 195, 130 }},
        { 195 , { 195, 131 }},
        { 196 , { 195, 132 }},
        { 197 , { 195, 133 }},
        { 198 , { 195, 134 }},
        { 199 , { 195, 135 }},
        { 200 , { 195, 136 }},
        { 201 , { 195, 137 }},
        { 202 , { 195, 138 }},
        { 203 , { 195, 139 }},
        { 204 , { 195, 140 }},
        { 205 , { 195, 141 }},
        { 206 , { 195, 142 }},
        { 207 , { 195, 143 }},
        { 208 , { 195, 144 }},
        { 209 , { 195, 145 }},
        { 210 , { 195, 146 }},
        { 211 , { 195, 147 }},
        { 212 , { 195, 148 }},
        { 213 , { 195, 149 }},
        { 214 , { 195, 150 }},
        { 215 , { 195, 151 }},
        { 216 , { 195, 152 }},
        { 217 , { 195, 153 }},
        { 218 , { 195, 154 }},
        { 219 , { 195, 155 }},
        { 220 , { 195, 156 }},
        { 221 , { 195, 157 }},
        { 222 , { 195, 158 }},
        { 223 , { 195, 159 }},
        { 224 , { 195, 160 }},
        { 225 , { 195, 161 }},
        { 226 , { 195, 162 }},
        { 227 , { 195, 163 }},
        { 228 , { 195, 164 }},
        { 229 , { 195, 165 }},
        { 230 , { 195, 166 }},
        { 231 , { 195, 167 }},
        { 232 , { 195, 168 }},
        { 233 , { 195, 169 }},
        { 234 , { 195, 170 }},
        { 235 , { 195, 171 }},
        { 236 , { 195, 172 }},
        { 237 , { 195, 173 }},
        { 238 , { 195, 174 }},
        { 239 , { 195, 175 }},
        { 240 , { 195, 176 }},
        { 241 , { 195, 177 }},
        { 242 , { 195, 178 }},
        { 243 , { 195, 179 }},
        { 244 , { 195, 180 }},
        { 245 , { 195, 181 }},
        { 246 , { 195, 182 }},
        { 247 , { 195, 183 }},
        { 248 , { 195, 184 }},
        { 249 , { 195, 185 }},
        { 250 , { 195, 186 }},
        { 251 , { 195, 187 }},
        { 252 , { 195, 188 }},
        { 253 , { 195, 189 }},
        { 254 , { 195, 190 }},
        { 255 , { 195, 191 }},
        { 0 , { 196, 128 }},
        { 1 , { 196, 129 }},
        { 2 , { 196, 130 }},
        { 3 , { 196, 131 }},
        { 4 , { 196, 132 }},
        { 5 , { 196, 133 }},
        { 6 , { 196, 134 }},
        { 7 , { 196, 135 }},
        { 8 , { 196, 136 }},
        { 9 , { 196, 137 }},
        { 10 , { 196, 138 }},
        { 11 , { 196, 139 }},
        { 12 , { 196, 140 }},
        { 13 , { 196, 141 }},
        { 14 , { 196, 142 }},
        { 15 , { 196, 143 }},
        { 16 , { 196, 144 }},
        { 17 , { 196, 145 }},
        { 18 , { 196, 146 }},
        { 19 , { 196, 147 }},
        { 20 , { 196, 148 }},
        { 21 , { 196, 149 }},
        { 22 , { 196, 150 }},
        { 23 , { 196, 151 }},
        { 24 , { 196, 152 }},
        { 25 , { 196, 153 }},
        { 26 , { 196, 154 }},
        { 27 , { 196, 155 }},
        { 28 , { 196, 156 }},
        { 29 , { 196, 157 }},
        { 30 , { 196, 158 }},
        { 31 , { 196, 159 }},
        { 32 , { 196, 160 }},
        { 127 , { 196, 161 }},
        { 128 , { 196, 162 }},
        { 129 , { 196, 163 }},
        { 130 , { 196, 164 }},
        { 131 , { 196, 165 }},
        { 132 , { 196, 166 }},
        { 133 , { 196, 167 }},
        { 134 , { 196, 168 }},
        { 135 , { 196, 169 }},
        { 136 , { 196, 170 }},
        { 137 , { 196, 171 }},
        { 138 , { 196, 172 }},
        { 139 , { 196, 173 }},
        { 140 , { 196, 174 }},
        { 141 , { 196, 175 }},
        { 142 , { 196, 176 }},
        { 143 , { 196, 177 }},
        { 144 , { 196, 178 }},
        { 145 , { 196, 179 }},
        { 146 , { 196, 180 }},
        { 147 , { 196, 181 }},
        { 148 , { 196, 182 }},
        { 149 , { 196, 183 }},
        { 150 , { 196, 184 }},
        { 151 , { 196, 185 }},
        { 152 , { 196, 186 }},
        { 153 , { 196, 187 }},
        { 154 , { 196, 188 }},
        { 155 , { 196, 189 }},
        { 156 , { 196, 190 }},
        { 157 , { 196, 191 }},
        { 158 , { 197, 128 }},
        { 159 , { 197, 129 }},
        { 160 , { 197, 130 }},
        { 173 , { 197, 131 }}
    };
}

void BytesToChars::validate_and_infer_types() {
    check_ragged_string_input(this, 0);
//    check_string_input(this, 5);
    set_ragged_string_output(this, 0, get_input_partial_shape(0));
}

bool BytesToChars::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    auto ragged_begins = inputs[0].data<const int32_t>();
    auto ragged_ends   = inputs[1].data<const int32_t>();
    auto begins = inputs[2].data<const int32_t>();
    auto ends   = inputs[3].data<const int32_t>();
    auto chars  = inputs[4].data<const uint8_t>();

    OPENVINO_ASSERT(inputs.size() == 5, "Too few inputs passed to BytesToChars, it means it is not converted properly or it is not used in the supported pattern");

    // Set output shapes
    outputs[0] = inputs[0];
    outputs[1] = inputs[1];
    outputs[2].set_shape(inputs[2].get_shape());
    outputs[3].set_shape(inputs[3].get_shape());
    outputs[4].set_shape(Shape({inputs[4].get_size() * 2}));
    const size_t num_elems = inputs[0].get_size();

    // Get pointers in the output tensors
    auto new_begins = outputs[2].data<int32_t>();
    auto new_ends   = outputs[3].data<int32_t>();
    auto new_chars  = outputs[4].data<uint8_t>();
    uint32_t char_pointer = 0;

    for(size_t j = 0; j < num_elems; ++j) {
        new_begins[j] = char_pointer;

        for(size_t i = ragged_begins[j]; i < ragged_ends[j]; ++i) {
            const auto word_len = ends[i] - begins[i];
            for (size_t k = 0; k < word_len; ++k) {
                for (auto byte : m_bytes_to_chars.at(chars[begins[i] + k])) {
                    new_chars[char_pointer++] = static_cast<int> (byte);
                }
            }
        }
        new_ends[j] = char_pointer;
    }

//    std::cerr << "Char pointer: " << char_pointer << "; old chars size: " << inputs[4].get_size() << "\n";
//
//    std::cerr << "Before set_shape:\n";
//    for (size_t i = 0; i < char_pointer; ++i) {
//        std::cerr << outputs[4].data<uint8_t>()[i] << ", ";
//    }
//    std::cerr << "\n";
//
//    for (size_t i = 0; i < char_pointer; ++i) {
//        std::cerr << static_cast<int>(outputs[4].data<uint8_t>()[i]) << ", ";
//    }
//    std::cerr << "\n";
//
//    outputs[4].set_shape({char_pointer});
//
//    std::cerr << "After set_shape:\n";
//
//    for (size_t i = 0; i < char_pointer; ++i) {
//        std::cerr << outputs[4].data<uint8_t>()[i] << ", ";
//    }
//    std::cerr << "\n";
//
//    for (size_t i = 0; i < char_pointer; ++i) {
//        std::cerr << static_cast<int>(outputs[4].data<uint8_t>()[i]) << ", ";
//    }
//    std::cerr << "\n";

    return true;
}


void WordpieceTokenizer::validate_and_infer_types() {
    check_ragged_string_input(this, 0);
    check_string_input(this, 5);
    set_ragged_output(this, 0, get_input_partial_shape(0), element::i32);
}

bool WordpieceTokenizer::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    auto ragged_begins = inputs[0].data<const int32_t>();
    auto ragged_ends   = inputs[1].data<const int32_t>();
    auto begins = inputs[2].data<const int32_t>();
    auto ends   = inputs[3].data<const int32_t>();
    auto chars  = inputs[4].data<const uint8_t>();

    auto vocab_begins = inputs[5].data<const int32_t>();
    auto vocab_ends   = inputs[6].data<const int32_t>();
    auto vocab_chars  = inputs[7].data<const uint8_t>();

    auto vocab_size = inputs[5].get_size();

    OPENVINO_ASSERT(inputs.size() == 9, "Too few inputs passed to WordpieceTokenizer, it means it is not converted properly or it is not used in the supported pattern");

    auto unk_token_id  = *inputs[8].data<const int32_t>();
    //std::cerr << "[ WordpieceTokenizer ] unk_token_id = " << unk_token_id << "\n";

#if 1

    // Set output shapes
    outputs[0].set_shape(inputs[0].get_shape());
    outputs[1].set_shape(inputs[1].get_shape());
    const size_t num_elems = inputs[0].get_size();

    //const size_t num_parts = inputs[2].get_size();
    //size_t new_num_parts = num_parts;

    // FIXME: Not accurate estimation as there is theoretical possibility for re-use the same symbol area
    // to represent different elements in ragged tensor
    outputs[2].set_shape({inputs[4].get_size()});

    // Get pointers in the output tensors
    auto new_begins = outputs[0].data<int32_t>();
    auto new_ends   = outputs[1].data<int32_t>();
    auto new_elems  = outputs[2].data<int32_t>();
    int32_t offset = 0;

    using namespace paddlenlp::fast_tokenizer;

    //std::cerr << "[ WordpieceTokenizer ] Start vocab reading\n";
    core::Vocab vocab;
    std::string unk_token;
    if(unk_token_id < 0)
        unk_token_id += vocab_size;
    for(size_t id = 0; id < vocab_size; ++id) {
        auto token = std::string(vocab_chars + vocab_begins[id], vocab_chars + vocab_ends[id]);
        vocab[token] = int32_t(id); // TODO: Check range
        if(id == unk_token_id)
            unk_token = token;
    }

    //std::cerr << "[ WordpieceTokenizer ] Finish vocab reading\n";
    //std::cerr << "[ WordpieceTokenizer ] unk_token = " << unk_token << "\n";
    //std::cerr << "[ WordpieceTokenizer ] Start tokenizer initialization\n";

    auto tokenizer = models::FastWordPiece(vocab, unk_token, m_max_bytes_per_word, m_suffix_indicator, true);   // FIXME: why true?

    //std::cerr << "[ WordpieceTokenizer ] Finish tokenizer initialization\n";


    for(size_t j = 0; j < num_elems; ++j) {
        new_begins[j] = offset;

        for(size_t i = ragged_begins[j]; i < ragged_ends[j]; ++i) {

            auto str = std::string(chars + begins[i], chars + ends[i]);
            std::vector<core::Token> results = tokenizer.Tokenize(str);

            for (const core::Token& token : results) {
                //std::cout << "[ WordpieceTokenizer ]     id: " << token.id_ << ", value: " << token.value_
                //          << ", offset: (" << token.offset_.first << ", "
                //          << token.offset_.second << ")." << std::endl;
                OPENVINO_ASSERT(offset < outputs[2].get_size());
                new_elems[offset++] = token.id_;
            };
        }

        new_ends[j] = offset;
    }

    outputs[2].set_shape({offset});

    OPENVINO_ASSERT(offset == outputs[2].get_size(), "Internal error in RegexSplit::evaluate: out of range for ragged parts");
    return true;

#else
    // Stub implementation that transforms each input string to its length duplicating element if the length is odd
    {
        std::cout << "[ DEBUG ] WordpieceTokenizer\n";
        std::cout << "[ DEBUG ]     vocab size: " << inputs[5].get_size() << "\n";
        std::cout << "[ DEBUG ]     unk_token_id: " << unk_token_id << "\n";

        // Set output shapes
        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());
        const size_t num_elems = inputs[0].get_size();

        const size_t num_parts = inputs[2].get_size();
        size_t new_num_parts = num_parts;
        // Count number of output elements
        for(size_t i = 0; i < num_parts; ++i) {
            auto length = ends[i] - begins[i];
            new_num_parts += length % 2;
        }

        outputs[2].set_shape({new_num_parts});

        // Get pointers in the output tensors
        auto new_begins = outputs[0].data<int32_t>();
        auto new_ends   = outputs[1].data<int32_t>();
        auto new_elems  = outputs[2].data<int32_t>();
        int32_t offset = 0;

        for(size_t j = 0; j < num_elems; ++j) {
            new_begins[j] = offset;

            for(size_t i = ragged_begins[j]; i < ragged_ends[j]; ++i) {

                auto length = ends[i] - begins[i];
                new_elems[offset++] = length;

                if(length % 2) {
                    new_elems[offset++] = length;
                }
            }

            new_ends[j] = offset;
        }

        OPENVINO_ASSERT(offset == outputs[2].get_size(), "Internal error in RegexSplit::evaluate: out of range for ragged parts");
        return true;
    }
    // End of stub implementation
#endif
}


ov::OutputVector translate_wordpiece_tokenize_with_offsets(const ov::frontend::NodeContext& node) {
    FRONT_END_GENERAL_CHECK(node.get_input_size() == 2, "WordpieceTokenizeWithOffsets expects 2 inputs");
    ov::OutputVector inputs = pre_translate_ragged_string_tensor_input(node.get_input(0));

    #if USE_STRING_TENSORS
    // It may seem enough to call pre_translate_string_tensor_input that will override Parameter element
    // type in case if string tensors are not used.
    // But a Parameter is still required to be overridden even if string tensors are used because in TF model
    // it is represented not as a string tensor, but as a resource with hash table for lookup that we cannot interpret
    // and have to replace by 1D string tensor.
    override_parameter(node.get_input(1).get_node_shared_ptr(), element::string, PartialShape{Dimension()});
    #endif

    auto vocab = pre_translate_string_tensor_input(node.get_input(1));
    inputs.insert(inputs.end(), vocab.begin(), vocab.end());
    // FIXME: Cannot set real value for unk_token_id from attributes because it is not known in this operation
    // TODO: Set other attributes.
    auto wp_tokenizer = std::make_shared<WordpieceTokenizer>(
        inputs,
        node.get_attribute<std::string>("suffix_indicator"),
        node.get_attribute<long>("max_bytes_per_word")
    );
    return { post_translate_ragged_tensor_output(wp_tokenizer->outputs()) };
}


void BPETokenizer::validate_and_infer_types() {
    check_ragged_string_input(this, 0);
    check_string_input(this, 5);
    check_string_input(this, 8);
    set_ragged_output(this, 0, get_input_partial_shape(0), element::i32);
}

bool BPETokenizer::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    auto ragged_begins = inputs[0].data<const int32_t>();
    auto ragged_ends   = inputs[1].data<const int32_t>();
    auto begins = inputs[2].data<const int32_t>();
    auto ends   = inputs[3].data<const int32_t>();
    auto chars  = inputs[4].data<const uint8_t>();

    auto vocab_begins = inputs[5].data<const int32_t>();
    auto vocab_ends   = inputs[6].data<const int32_t>();
    auto vocab_chars  = inputs[7].data<const uint8_t>();

    auto merges_begins = inputs[8].data<const int32_t>();
    auto merges_ends   = inputs[9].data<const int32_t>();
    auto merges_chars  = inputs[10].data<const uint8_t>();

    auto vocab_size = inputs[5].get_size();
    auto merges_size = inputs[8].get_size();

    OPENVINO_ASSERT(inputs.size() == 11, "Too few inputs passed to BPETokenizer, it means it is not converted properly or it is not used in the supported pattern");

#if 1
    // Set output shapes
    outputs[0].set_shape(inputs[0].get_shape());
    outputs[1].set_shape(inputs[1].get_shape());
    const size_t num_elems = inputs[0].get_size();

    // FIXME: Not accurate estimation as there is theoretical possibility for re-use the same symbol area
    // to represent different elements in ragged tensor
    outputs[2].set_shape({inputs[4].get_size()});

    using namespace paddlenlp::fast_tokenizer;

    std::cerr << "[ BPETokenizer ] Start vocab reading\n";
    core::Vocab vocab;
    int32_t unk_token_id = -1;

    std::cerr << "[ BPETokenizer ] Vocab size is " << vocab_size << "\n";

    for(size_t id = 0; id < vocab_size; ++id) {
        auto token = std::string(vocab_chars + vocab_begins[id], vocab_chars + vocab_ends[id]);
        vocab[token] = int32_t(id); // TODO: Check range
    }

    std::cerr << "[ BPETokenizer ] Finish vocab reading\n";

    std::cerr << "[ BPETokenizer ] Start merges reading\n";
    std::cerr << "[ BPETokenizer ] Merges Size: " << merges_size << "\n";
    core::Merges merges;
    std::string delim = " ";


    for(size_t id = 0; id < merges_size; ++id) {
        auto merge = std::string(merges_chars + merges_begins[id], merges_chars + merges_ends[id]);
        const int delim_pos = merge.find(delim);

        std::pair<std::string, std::string> merge_pair = {
            merge.substr(0, delim_pos), merge.substr(delim_pos + 1)
        };
        merges.emplace_back(merge_pair);
    }

    std::cerr << "[ BPETokenizer ] Finish merges reading\n";


    std::cerr << "[ BPETokenizer ] Start tokenizer initialization\n";

    std::vector<std::string> unk_token = {};
    if (m_unk_token.size() > 0) {
        unk_token.push_back(m_unk_token);
    };
    std::vector<std::string> suffix_indicator = {};
    if (m_suffix_indicator.size() > 0) {
        suffix_indicator.push_back(m_suffix_indicator);
    };
    std::vector<std::string> end_suffix = {};
    if (m_end_suffix.size() > 0) {
        end_suffix.push_back(m_end_suffix);
    };

    models::BPE tokenizer(vocab, merges, 10000 /* default cache size */, {} /* dropout - don't use dropout for inference */,
                          unk_token, suffix_indicator, end_suffix, m_fuse_unk);

    std::cerr << "[ BPETokenizer ] Finish tokenizer initialization\n";

    // Get pointers in the output tensors
    auto new_begins = outputs[0].data<int32_t>();
    auto new_ends   = outputs[1].data<int32_t>();
    auto new_elems  = outputs[2].data<int32_t>();
    int32_t offset = 0;


    for(size_t j = 0; j < num_elems; ++j) {
        new_begins[j] = offset;
        for(size_t i = ragged_begins[j]; i < ragged_ends[j]; ++i) {
            auto str = std::string(chars + begins[i], chars + ends[i]);

            std::cerr << "Word: '" << str << "'\n";
            std::vector<core::Token> results = tokenizer.Tokenize(str);

            for (const core::Token& token : results) {
                std::cout << "[ BPETokenizer ]     id: " << token.id_ << ", value: " << token.value_
                          << ", offset: (" << token.offset_.first << ", "
                          << token.offset_.second << ")." << std::endl;
                OPENVINO_ASSERT(offset < outputs[2].get_size());
                new_elems[offset++] = token.id_;
            };
        }

        new_ends[j] = offset;
    }

    outputs[2].set_shape({offset});

    OPENVINO_ASSERT(offset == outputs[2].get_size(), "Internal error in RegexSplit::evaluate: out of range for ragged parts");
    return true;

#else
    // Stub implementation that transforms each input string to its length duplicating element if the length is odd
    {
        std::cout << "[ DEBUG ] WordpieceTokenizer\n";
        std::cout << "[ DEBUG ]     vocab size: " << inputs[5].get_size() << "\n";
        std::cout << "[ DEBUG ]     unk_token_id: " << unk_token_id << "\n";

        // Set output shapes
        outputs[0].set_shape(inputs[0].get_shape());
        outputs[1].set_shape(inputs[1].get_shape());
        const size_t num_elems = inputs[0].get_size();

        const size_t num_parts = inputs[2].get_size();
        size_t new_num_parts = num_parts;
        // Count number of output elements
        for(size_t i = 0; i < num_parts; ++i) {
            auto length = ends[i] - begins[i];
            new_num_parts += length % 2;
        }

        outputs[2].set_shape({new_num_parts});

        // Get pointers in the output tensors
        auto new_begins = outputs[0].data<int32_t>();
        auto new_ends   = outputs[1].data<int32_t>();
        auto new_elems  = outputs[2].data<int32_t>();
        int32_t offset = 0;

        for(size_t j = 0; j < num_elems; ++j) {
            new_begins[j] = offset;

            for(size_t i = ragged_begins[j]; i < ragged_ends[j]; ++i) {

                auto length = ends[i] - begins[i];
                new_elems[offset++] = length;

                if(length % 2) {
                    new_elems[offset++] = length;
                }
            }

            new_ends[j] = offset;
        }

        OPENVINO_ASSERT(offset == outputs[2].get_size(), "Internal error in RegexSplit::evaluate: out of range for ragged parts");
        return true;
    }
    // End of stub implementation
#endif
}


ov::OutputVector translate_lookup_table_find_v2(const ov::frontend::NodeContext& node) {
    FRONT_END_GENERAL_CHECK(node.get_input_size() == 3, "LookupTableFindV2 expects 3 inputs");

    // Check if this node is used in a combination with already converted WordpieceTokenizeWithOffsets
    auto wp_tokenizer_outputs = pre_translate_ragged_tensor_input(node.get_input(1));
    auto wp_tokenizer = dynamic_cast<WordpieceTokenizer*>(wp_tokenizer_outputs[0].get_node());
    OPENVINO_ASSERT(wp_tokenizer, "Conversion of LookupTableFindV2 without coupled WordpieceTokenizer is not yet supported");

    // TODO: Check vocab matching for LookupTableFindV2 and WordpieceTokenizer

    // TODO: Check if overflow really happens in real models due to i64 to i32 conversion
    auto unk_token_id = std::make_shared<opset10::Convert>(node.get_input(2), element::i32);

    auto wp_tokenizer_inputs = wp_tokenizer->input_values();
    wp_tokenizer_inputs.push_back(unk_token_id);
    //std::cerr << "Added extra input, total number of inputs is " << wp_tokenizer_inputs.size() << "\n";

    auto new_wp_tokenizer = wp_tokenizer->clone_with_new_inputs(wp_tokenizer_inputs);
    return { post_translate_ragged_tensor_output(new_wp_tokenizer->outputs()) };
}


void RaggedToDense::validate_and_infer_types() {
    OPENVINO_ASSERT(get_input_size() == 3 + 1 + 1);

    // Input ragged tensor
    check_ragged_input(this, 0);

    // Target size along ragged dimension
    OPENVINO_ASSERT(get_input_element_type(3).is_integral_number());
    auto rank = get_input_partial_shape(3).rank();
    OPENVINO_ASSERT(
        rank.is_dynamic() ||
        rank.get_length() == 0 ||
        rank.get_length() == 1 && get_input_partial_shape(3)[0].compatible(1),
        "Target dense dimension size for RaggedToDense should be a 0D or 1D tensor with a single element");

    // Default value to fill out of ragged range elements in output tensor
    OPENVINO_ASSERT(get_input_element_type(4).compatible(get_input_element_type(2)));
    auto input4_rank = get_input_partial_shape(4).rank();
    OPENVINO_ASSERT(input4_rank.compatible(0));

    set_input_is_relevant_to_shape(3);

    if(get_input_partial_shape(0).rank().is_dynamic()) {
        set_output_type(0, get_input_element_type(2), PartialShape::dynamic());
        set_output_type(1, element::boolean, PartialShape::dynamic());
    } else {
        auto shape = get_input_partial_shape(0);
        if(auto target_dim = dynamic_cast<Constant*>(get_input_node_ptr(3))) {
            shape.push_back(target_dim->cast_vector<int64_t>()[0]);
        } else {
            shape.push_back(Dimension());
        }
            set_output_type(0, get_input_element_type(2), shape);
            set_output_type(1, element::boolean, shape);
    }
}


bool RaggedToDense::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    // FIXME: Works for POD types only (not for strings!)
    // FIXME: Output mask is calculated even if there are no consumers
    auto begins = inputs[0].data<const int32_t>();
    auto ends   = inputs[1].data<const int32_t>();
    auto nelems = inputs[0].get_size();
    auto elems  = reinterpret_cast<const char*>(inputs[2].data());
    auto elem_size = inputs[2].get_element_type().size();
    auto default_value = reinterpret_cast<const char*>(inputs[4].data());

    // Suppose validate was called and set correct output shape
    // Take a target shape value for ragged dimension
    size_t target_dim = outputs[0].get_shape().back();

    auto out_elems = reinterpret_cast<char*>(outputs[0].data());
    auto out_mask = outputs[1].data<char>();

    auto out_elem_orig = out_elems;
    auto out_mask_orig = out_mask;

    for(size_t i = 0; i < nelems; ++i) {
        auto begin = elems + elem_size*begins[i];
        auto len = std::min(size_t(ends[i] - begins[i]), target_dim);  // truncation
        auto end = begin + elem_size*len;
        out_elems = std::copy(begin, end, out_elems);
        out_mask = std::fill_n(out_mask, len, char(1));
        if(len < target_dim)
            out_mask = std::fill_n(out_mask, target_dim - len, char(0));
        while(len < target_dim) {
            out_elems = std::copy(default_value, default_value + elem_size, out_elems);
            ++len;
        }
    }

    OPENVINO_ASSERT(out_elems == out_elem_orig + outputs[0].get_byte_size());
    OPENVINO_ASSERT(out_mask == out_mask_orig + outputs[1].get_byte_size());
    return true;
}

void CombineSegments::validate_and_infer_types() {
    OPENVINO_ASSERT(get_input_size() > 0);
    OPENVINO_ASSERT((get_input_size() - 1)%3 == 0);

    // First come several ragged tensors each represented as 3 regular tesors
    size_t num_inputs = (get_input_size() - 1)/3;
    PartialShape ps = PartialShape::dynamic();
    element::Type et = element::dynamic;
    for (size_t i = 0; i < num_inputs; ++i) {
        check_ragged_input(this, 3*i);
        // Check limited broadcast
        // Limited means that we support only two shapes on inputs: scalar and not scalars,
        // and all not-scalars should have the same shape
        auto rank = get_input_partial_shape(3*i).rank();
        if(rank.is_static() && rank.get_length()) {
            OPENVINO_ASSERT(ps.merge_into(ps, get_input_partial_shape(3*i)));
        }
        OPENVINO_ASSERT(element::Type::merge(et, et, get_input_element_type(3*i)));
        OPENVINO_ASSERT(element::Type::merge(et, et, get_input_element_type(3*i + 1)));
    }

    //std::cerr << ps << '\n';

    set_ragged_output(this, 0, ps, et);
    // TODO: Avoid emitting ragged indices for the second ragged tensor, they should be identical to the first output ragged tensor
    set_ragged_output(this, 3, ps, get_input_element_type(get_input_size() - 1));
}


bool CombineSegments::evaluate(ov::TensorVector& outputs, const ov::TensorVector& inputs) const {
    // FIXME: Works for POD types only (not for strings!)
    size_t num_of_ragged = (inputs.size() - 1)/3;
    OPENVINO_ASSERT(num_of_ragged == inputs.back().get_size());
    std::vector<const int32_t*> begins;
    std::vector<const int32_t*> ends;
    std::vector<size_t> nelems;
    std::vector<const char*> elems;
    auto element_type = inputs[2].get_element_type();
    auto elem_size = element_type.size();
    size_t max_nelems = 0;
    size_t flat_out_size = 0;
    Shape ps;

    for(size_t i = 0; i < num_of_ragged; ++i) {
        OPENVINO_ASSERT(inputs[3*i + 2].get_element_type() == element_type);
        begins.push_back(inputs[3*i + 0].data<const int32_t>());
        ends.push_back(inputs[3*i + 1].data<const int32_t>());
        nelems.push_back(inputs[3*i + 0].get_size());
        //std::cerr << "inputs[3*i + 0].get_size() = " << inputs[3*i + 0].get_size() << "\n";
        elems.push_back(reinterpret_cast<const char*>(inputs[3*i + 2].data()));
        // TODO: Get rank from a tensor instead of partial_shape. This is a WA for CPU bug that gives 1D tensors instead of 0D tensors.
        if(get_input_partial_shape(3*i + 0).rank().get_length() > 0) {
            ps = inputs[3*i + 0].get_shape();
            //std::cerr << "updated\n";
        }
        //std::cerr << "ps = " << ps << "\nget_input_partial_shape(3*i) = " << get_input_partial_shape(3*i) << "\n";
        //OPENVINO_ASSERT(ps.merge_into(ps, get_input_partial_shape(3*i)));
        max_nelems = std::max(max_nelems, nelems.back());
    }

    // flat_out_size is going to be an estimation of the final size
    // This is only an estimation, not the exact output size, because ragged tensor may have gaps in the representation

    for(size_t i = 0; i < num_of_ragged; ++i) {
        //std::cerr << "max_nelems = " << max_nelems << "\n";
        if(nelems[i] == 1) {
            flat_out_size += max_nelems * inputs[3*i + 2].get_size(); // broadcast
        } else {
            flat_out_size += inputs[3*i + 2].get_size();    // FIXME: doesn't work for overlapped ragged regions
        }
    }

    auto ids = reinterpret_cast<const char*>(inputs.back().data());
    size_t id_type_size = inputs.back().get_element_type().size();

    outputs[3*0 + 0].set_shape(ps);
    outputs[3*0 + 1].set_shape(ps);
    OPENVINO_ASSERT(max_nelems == outputs[3*0 + 0].get_size());
    OPENVINO_ASSERT(max_nelems == outputs[3*0 + 1].get_size());
    outputs[3*0 + 2].set_shape({flat_out_size});

    outputs[3*1 + 0].set_shape(ps);
    outputs[3*1 + 1].set_shape(ps);
    OPENVINO_ASSERT(max_nelems == outputs[3*1 + 0].get_size());
    OPENVINO_ASSERT(max_nelems == outputs[3*1 + 1].get_size());
    outputs[3*1 + 2].set_shape({flat_out_size});

    auto out_elem_begins = outputs[3*0 + 0].data<int32_t>();
    auto out_elem_ends = outputs[3*0 + 1].data<int32_t>();
    auto out_elems = reinterpret_cast<char*>(outputs[3*0 + 2].data());
    auto out_id_begins = outputs[3*1 + 0].data<int32_t>();
    auto out_id_ends = outputs[3*1 + 1].data<int32_t>();
    auto out_ids = reinterpret_cast<char*>(outputs[3*1 + 2].data());

    auto out_elems_orig = out_elems;
    auto out_ids_orig = out_ids;
    size_t out_offset = 0;

    for(size_t i = 0; i < max_nelems; ++i) {
        out_elem_begins[i] = out_offset;
        out_id_begins[i] = out_offset;

        for(size_t j = 0; j < num_of_ragged; ++j) {
            const char* begin;
            size_t len;
            if(nelems[j] == 1) {
                begin = elems[j] + elem_size*begins[j][0];
                len = ends[j][0] - begins[j][0];
            } else {
                begin = elems[j] + elem_size*begins[j][i];
                len = ends[j][i] - begins[j][i];
            }
            auto end = begin + elem_size*len;
            out_elems = std::copy(begin, end, out_elems);
            for(size_t k = 0; k < len; ++k) {
                out_ids = std::copy(ids + id_type_size*j, ids + id_type_size*(j + 1), out_ids);
            }
            out_offset += len;
        }

        out_elem_ends[i] = out_offset;
        out_id_ends[i] = out_offset;
    }

    OPENVINO_ASSERT(out_offset <= flat_out_size);

    outputs[3*0 + 2].set_shape({out_offset});
    outputs[3*1 + 2].set_shape({out_offset});

    OPENVINO_ASSERT(out_elems == out_elems_orig + outputs[3*0 + 2].get_byte_size());
    OPENVINO_ASSERT(out_ids == out_ids_orig + outputs[3*1 + 2].get_byte_size());
    return true;
}


ov::OutputVector translate_reshape(const ov::frontend::NodeContext& node) {
    // This is a copied-and-pasted and adopted fragment of TF reshape translator from OV.
    // It checks if the input tensor has string type, and then perform custom tranlation.
    // Otherwise it should operate identically to the stock version of Reshape translator in TF FE.
    // TODO: Introduce an API to call original translators from an extension without copying the code to an extension.

    FRONT_END_GENERAL_CHECK(node.get_input_size() == 2, "Tensorflow Reshape op should have two inputs");
    auto tensor = node.get_input(0);
    auto shape = node.get_input(1);
    if(auto pack = dynamic_cast<StringTensorPack*>(tensor.get_node())) {
        // TODO: If it is a beginning of the graph, how to detect strings? It falls in 'else' branch in this case.
        // FIXME: Needs extension for a Parameter to prepare it first
        auto begins = std::make_shared<Reshape>(pack->input_value(0), shape, false);
        auto ends = std::make_shared<Reshape>(pack->input_value(1), shape, false);
        auto chars = pack->input_value(2);
        auto reshape = post_translate_string_tensor_output({begins, ends, chars});
        return {reshape};
    } else {
        auto reshape = std::make_shared<Reshape>(tensor, shape, false);
        return {reshape};
    }
    // set_node_name(node.get_name(), reshape); // TODO: requires dependencies from TF FE internals
}


// Copied and pasted from TF FE and adopted to not use internal TF FE operation classes
ov::OutputVector translate_const(const ov::frontend::NodeContext& node) {
    auto ov_type = node.get_attribute_as_any("dtype");
    std::shared_ptr<Node> const_node;
    if (!ov_type.is<ov::element::Type>() || ov_type.as<ov::element::Type>() == ov::element::dynamic ||
        ov_type.as<ov::element::Type>() == ov::element::undefined) {
        if (ov_type.is<std::string>() && ov_type.as<std::string>() == "DT_STRING") {
            auto value_as_any = node.get_attribute_as_any("value");
            const auto& values = value_as_any.as<std::vector<std::string>>();
            ov::Tensor begins(element::i32, {}), ends(element::i32, {}), chars(element::u8, {});
            unpack_strings(&values[0], {values.size()}, begins, ends, chars);
            const_node = std::make_shared<StringTensorPack>(OutputVector{
                std::make_shared<Constant>(begins),
                std::make_shared<Constant>(ends),
                std::make_shared<Constant>(chars)
            });
        } else {
            const_node = std::make_shared<ov::op::util::FrameworkNode>(OutputVector{});
        }
    } else {
        //static std::vector<ov::Tensor> tensors;
        auto tensor = node.get_attribute<ov::Tensor>("value");
        //tensors.push_back(tensor);
        const_node = std::make_shared<Constant>(tensor);
        #if OPENVINO_ELEMENT_STRING_SUPPORTED
        if (const_node->get_element_type() == element::string) {
            if(shape_size(tensor.get_shape()) > 0) {
                auto strings = std::dynamic_pointer_cast<Constant>(const_node)->get_data_ptr<std::string>();
            }
            const_node = std::make_shared<StringTensorUnpack>(const_node->outputs());
            const_node = std::make_shared<StringTensorPack>(const_node->outputs());
        }
        #endif
    }
    //set_node_name(node.get_name(), const_node);   // TODO: Provide alternative to internal function set_node_name
    return {const_node};
}

