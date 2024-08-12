// sherpa-onnx/csrc/online-transducer-modified-beam-search-decoder.cc
//
// Copyright (c)  2023  Pingfeng Luo
// Copyright (c)  2023  Xiaomi Corporation

#include "sherpa-onnx/csrc/online-transducer-modified-beam-search-decoder.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <chrono>  // 追加
#include <ctime>   // 追加

#include "sherpa-onnx/csrc/log.h"
#include "sherpa-onnx/csrc/onnx-utils.h"

namespace sherpa_onnx {

static void UseCachedDecoderOut(
    const std::vector<int32_t> &hyps_row_splits,
    const std::vector<OnlineTransducerDecoderResult> &results,
    Ort::Value *decoder_out) {
  std::vector<int64_t> shape =
      decoder_out->GetTensorTypeAndShapeInfo().GetShape();

  float *dst = decoder_out->GetTensorMutableData<float>();

  int32_t batch_size = static_cast<int32_t>(results.size());
  for (int32_t i = 0; i != batch_size; ++i) {
    int32_t num_hyps = hyps_row_splits[i + 1] - hyps_row_splits[i];
    if (num_hyps > 1 || !results[i].decoder_out) {
      dst += num_hyps * shape[1];
      continue;
    }

    const float *src = results[i].decoder_out.GetTensorData<float>();
    std::copy(src, src + shape[1], dst);
    dst += shape[1];
  }
}

OnlineTransducerDecoderResult
OnlineTransducerModifiedBeamSearchDecoder::GetEmptyResult() const {
  int32_t context_size = model_->ContextSize();
  int32_t blank_id = 0;  // always 0
  OnlineTransducerDecoderResult r;
  std::vector<int64_t> blanks(context_size, -1);
  blanks.back() = blank_id;

  Hypotheses blank_hyp({{blanks, 0}});
  r.hyps = std::move(blank_hyp);
  r.tokens = std::move(blanks);
  return r;
}

void OnlineTransducerModifiedBeamSearchDecoder::StripLeadingBlanks(
    OnlineTransducerDecoderResult *r) const {
  int32_t context_size = model_->ContextSize();
  auto hyp = r->hyps.GetMostProbable(true);

  std::vector<int64_t> tokens(hyp.ys.begin() + context_size, hyp.ys.end());
  r->tokens = std::move(tokens);
  r->timestamps = std::move(hyp.timestamps);

  // export per-token scores
  r->ys_probs = std::move(hyp.ys_probs);
  r->lm_probs = std::move(hyp.lm_probs);
  r->context_scores = std::move(hyp.context_scores);

  r->num_trailing_blanks = hyp.num_trailing_blanks;
}

void OnlineTransducerModifiedBeamSearchDecoder::Decode(
    Ort::Value encoder_out,
    std::vector<OnlineTransducerDecoderResult> *result) {
  Decode(std::move(encoder_out), nullptr, result);
}

void OnlineTransducerModifiedBeamSearchDecoder::Decode(
    Ort::Value encoder_out, OnlineStream **ss,
    std::vector<OnlineTransducerDecoderResult> *result) {
  using Clock = std::chrono::high_resolution_clock;  // 高精度時計のエイリアス

  auto start_total = Clock::now();

  std::vector<int64_t> encoder_out_shape =
      encoder_out.GetTensorTypeAndShapeInfo().GetShape();

  if (static_cast<int32_t>(encoder_out_shape[0]) !=
      static_cast<int32_t>(result->size())) {
    SHERPA_ONNX_LOGE(
        "Size mismatch! encoder_out.size(0) %d, result.size(0): %d\n",
        static_cast<int32_t>(encoder_out_shape[0]),
        static_cast<int32_t>(result->size()));
    exit(-1);
  }

  int32_t batch_size = static_cast<int32_t>(encoder_out_shape[0]);
  int32_t num_frames = static_cast<int32_t>(encoder_out_shape[1]);
  int32_t vocab_size = model_->VocabSize();

  std::vector<Hypotheses> cur;
  for (auto &r : *result) {
    cur.push_back(std::move(r.hyps));
  }
  std::vector<Hypothesis> prev;

  for (int32_t t = 0; t != num_frames; ++t) {

    auto hyps_row_splits = GetHypsRowSplits(cur);
    int32_t num_hyps = hyps_row_splits.back();
    prev.clear();
    for (auto &hyps : cur) {
      for (auto &h : hyps) {
        prev.push_back(std::move(h.second));
      }
    }
    cur.clear();
    cur.reserve(batch_size);

    auto start_decoder = Clock::now();
    Ort::Value decoder_input = model_->BuildDecoderInput(prev);
    Ort::Value decoder_out = model_->RunDecoder(std::move(decoder_input));
    if (t == 0) {
      UseCachedDecoderOut(hyps_row_splits, *result, &decoder_out);
    }
    auto end_decoder = Clock::now();
    std::chrono::duration<double> decoder_time = end_decoder - start_decoder;
    SHERPA_ONNX_LOGE("Decoder time for frame %d: %f seconds.", t, decoder_time.count());

    auto start_joiner = Clock::now();
    Ort::Value cur_encoder_out = GetEncoderOutFrame(model_->Allocator(), &encoder_out, t);
    cur_encoder_out = Repeat(model_->Allocator(), &cur_encoder_out, hyps_row_splits);
    Ort::Value logit = model_->RunJoiner(std::move(cur_encoder_out), View(&decoder_out));
    auto end_joiner = Clock::now();
    std::chrono::duration<double> joiner_time = end_joiner - start_joiner;
    SHERPA_ONNX_LOGE("Joiner time for frame %d: %f seconds.", t, joiner_time.count());

    auto start_beam = Clock::now();
    float *p_logit = logit.GetTensorMutableData<float>();

    int32_t p_logit_items = vocab_size * num_hyps;
    std::vector<float> logit_with_temperature(p_logit_items);
    {
      std::copy(p_logit, p_logit + p_logit_items, logit_with_temperature.begin());
      for (float &elem : logit_with_temperature) {
        elem /= temperature_scale_;
      }
      LogSoftmax(logit_with_temperature.data(), vocab_size, num_hyps);
    }

    if (blank_penalty_ > 0.0) {
      SubtractBlank(p_logit, vocab_size, num_hyps, 0, blank_penalty_);
    }
    LogSoftmax(p_logit, vocab_size, num_hyps);

    float *p_logprob = p_logit;

    for (int32_t i = 0; i != num_hyps; ++i) {
      float log_prob = prev[i].log_prob + prev[i].lm_log_prob;
      for (int32_t k = 0; k != vocab_size; ++k, ++p_logprob) {
        *p_logprob += log_prob;
      }
    }
    p_logprob = p_logit;

    for (int32_t b = 0; b != batch_size; ++b) {
      int32_t frame_offset = (*result)[b].frame_offset;
      int32_t start = hyps_row_splits[b];
      int32_t end = hyps_row_splits[b + 1];
      auto topk = TopkIndex(p_logprob, vocab_size * (end - start), max_active_paths_);

      Hypotheses hyps;
      for (auto k : topk) {
        int32_t hyp_index = k / vocab_size + start;
        int32_t new_token = k % vocab_size;

        Hypothesis new_hyp = prev[hyp_index];
        const float prev_lm_log_prob = new_hyp.lm_log_prob;
        float context_score = 0;
        auto context_state = new_hyp.context_state;

        if (new_token != 0 && new_token != unk_id_) {
          new_hyp.ys.push_back(new_token);
          new_hyp.timestamps.push_back(t + frame_offset);
          new_hyp.num_trailing_blanks = 0;
          if (ss != nullptr && ss[b]->GetContextGraph() != nullptr) {
            auto context_res = ss[b]->GetContextGraph()->ForwardOneStep(context_state, new_token, false);
            context_score = std::get<0>(context_res);
            new_hyp.context_state = std::get<1>(context_res);
          }
          if (lm_) {
            lm_->ComputeLMScore(lm_scale_, &new_hyp);
          }
        } else {
          ++new_hyp.num_trailing_blanks;
        }
        new_hyp.log_prob = p_logprob[k] + context_score - prev_lm_log_prob;

        if (new_token != 0 && new_token != unk_id_) {
          float y_prob = logit_with_temperature[start * vocab_size + k];
          new_hyp.ys_probs.push_back(y_prob);

          if (lm_) {
            float lm_prob = new_hyp.lm_log_prob - prev_lm_log_prob;
            if (lm_scale_ != 0.0) {
              lm_prob /= lm_scale_;
            }
            new_hyp.lm_probs.push_back(lm_prob);
          }

          if (ss != nullptr && ss[b]->GetContextGraph() != nullptr) {
            new_hyp.context_scores.push_back(context_score);
          }
        }

        hyps.Add(std::move(new_hyp));
      }
      cur.push_back(std::move(hyps));
      p_logprob += (end - start) * vocab_size;
    }

    auto end_beam = Clock::now();
    std::chrono::duration<double> frame_time = end_beam - start_beam;
    SHERPA_ONNX_LOGE("Processing time for beam search %d: %f seconds.", t, frame_time.count());
  }

  for (int32_t b = 0; b != batch_size; ++b) {
    auto &hyps = cur[b];
    auto best_hyp = hyps.GetMostProbable(true);
    auto &r = (*result)[b];

    r.hyps = std::move(hyps);
    r.tokens = std::move(best_hyp.ys);
    r.num_trailing_blanks = best_hyp.num_trailing_blanks;
    r.frame_offset += num_frames;
  }

  auto end_total = Clock::now();
  std::chrono::duration<double> total_time = end_total - start_total;
  SHERPA_ONNX_LOGE("Total Decode time: %f seconds.", total_time.count());
}
void OnlineTransducerModifiedBeamSearchDecoder::UpdateDecoderOut(
    OnlineTransducerDecoderResult *result) {
  if (static_cast<int32_t>(result->tokens.size()) == model_->ContextSize()) {
    result->decoder_out = Ort::Value{nullptr};
    return;
  }
  Ort::Value decoder_input = model_->BuildDecoderInput({*result});
  result->decoder_out = model_->RunDecoder(std::move(decoder_input));
}
}  // namespace sherpa_onnx
