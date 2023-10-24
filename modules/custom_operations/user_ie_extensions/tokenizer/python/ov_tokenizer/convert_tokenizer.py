# -*- coding: utf-8 -*-
# Copyright (C) 2018-2023 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import logging
import sys
from typing import Any, Tuple, Union

from openvino.runtime import Model
from openvino.runtime.exceptions import OVTypeError


logger = logging.getLogger(__name__)


def convert_tokenizer(
    tokenizer_object: Any, number_of_inputs: int = 1, with_decoder: bool = False, streaming_decoder: bool = False
) -> Union[Model, Tuple[Model, Model]]:
    # todo: add support for more then 1 input
    if number_of_inputs > 1:
        raise ValueError("Tokenizers with more then one input are not supported yet.")

    if "transformers" in sys.modules:
        from transformers import PreTrainedTokenizerBase, PreTrainedTokenizerFast

        from .hf_parser import convert_fast_tokenizer, convert_sentencepiece_model_tokenizer, is_sentencepiece_model

        if isinstance(tokenizer_object, PreTrainedTokenizerBase):
            if is_sentencepiece_model(tokenizer_object):
                logger.info("Convert tokenizer using SentencePiece .model file.")
                return convert_sentencepiece_model_tokenizer(
                    tokenizer_object,
                    add_attention_mask=True,
                    with_decoder=with_decoder,
                    streaming_decoder=streaming_decoder,
                )
            elif isinstance(tokenizer_object, PreTrainedTokenizerFast):
                logger.info("Convert Huggingface Fast tokenizer pipeline.")
                return convert_fast_tokenizer(
                    tokenizer_object,
                    number_of_inputs=number_of_inputs,
                    with_decoder=with_decoder,
                )

    raise OVTypeError(f"Tokenizer type is not supported: {type(tokenizer_object)}")