// sherpa-onnx/csrc/offline-recognizer-transducer-impl.h
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#ifndef SHERPA_ONNX_CSRC_OFFLINE_RECOGNIZER_TRANSDUCER_IMPL_H_
#define SHERPA_ONNX_CSRC_OFFLINE_RECOGNIZER_TRANSDUCER_IMPL_H_

#include <fstream>
#include <ios>
#include <memory>
#include <regex>  // NOLINT
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <chrono>  // 追加
#include <ctime>   // 追加

#if __ANDROID_API__ >= 9
#include "android/asset_manager.h"
#include "android/asset_manager_jni.h"
#endif

#include "sherpa-onnx/csrc/context-graph.h"
#include "sherpa-onnx/csrc/log.h"
#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/offline-recognizer-impl.h"
#include "sherpa-onnx/csrc/offline-recognizer.h"
#include "sherpa-onnx/csrc/offline-transducer-decoder.h"
#include "sherpa-onnx/csrc/offline-transducer-greedy-search-decoder.h"
#include "sherpa-onnx/csrc/offline-transducer-model.h"
#include "sherpa-onnx/csrc/offline-transducer-modified-beam-search-decoder.h"
#include "sherpa-onnx/csrc/pad-sequence.h"
#include "sherpa-onnx/csrc/symbol-table.h"
#include "sherpa-onnx/csrc/utils.h"
#include "ssentencepiece/csrc/ssentencepiece.h"

namespace sherpa_onnx {

static OfflineRecognitionResult Convert(
    const OfflineTransducerDecoderResult &src, const SymbolTable &sym_table,
    int32_t frame_shift_ms, int32_t subsampling_factor) {
  OfflineRecognitionResult r;
  r.tokens.reserve(src.tokens.size());
  r.timestamps.reserve(src.timestamps.size());

  std::string text;
  for (auto i : src.tokens) {
    auto sym = sym_table[i];
    text.append(sym);

    if (sym.size() == 1 && (sym[0] < 0x20 || sym[0] > 0x7e)) {
      // for byte bpe models,
      // (but don't rewrite printable characters 0x20..0x7e,
      //  which collide with standard BPE units)
      std::ostringstream os;
      os << "<0x" << std::hex << std::uppercase
         << (static_cast<int32_t>(sym[0]) & 0xff) << ">";
      sym = os.str();
    }

    r.tokens.push_back(std::move(sym));
  }
  r.text = std::move(text);

  float frame_shift_s = frame_shift_ms / 1000. * subsampling_factor;
  for (auto t : src.timestamps) {
    float time = frame_shift_s * t;
    r.timestamps.push_back(time);
  }

  return r;
}

class OfflineRecognizerTransducerImpl : public OfflineRecognizerImpl {
 public:
  explicit OfflineRecognizerTransducerImpl(
      const OfflineRecognizerConfig &config)
      : OfflineRecognizerImpl(config),
        config_(config),
        symbol_table_(config_.model_config.tokens),
        model_(std::make_unique<OfflineTransducerModel>(config_.model_config)) {
    if (symbol_table_.Contains("<unk>")) {
      unk_id_ = symbol_table_["<unk>"];
    }

    if (config_.decoding_method == "greedy_search") {
      decoder_ = std::make_unique<OfflineTransducerGreedySearchDecoder>(
          model_.get(), unk_id_, config_.blank_penalty);
    } else if (config_.decoding_method == "modified_beam_search") {
      if (!config_.lm_config.model.empty()) {
        lm_ = OfflineLM::Create(config.lm_config);
      }

      if (!config_.model_config.bpe_vocab.empty()) {
        bpe_encoder_ = std::make_unique<ssentencepiece::Ssentencepiece>(
            config_.model_config.bpe_vocab);
      }

      if (!config_.hotwords_file.empty()) {
        InitHotwords();
      }

      decoder_ = std::make_unique<OfflineTransducerModifiedBeamSearchDecoder>(
          model_.get(), lm_.get(), config_.max_active_paths,
          config_.lm_config.scale, unk_id_, config_.blank_penalty);
    } else {
      SHERPA_ONNX_LOGE("Unsupported decoding method: %s",
                       config_.decoding_method.c_str());
      exit(-1);
    }
  }

#if __ANDROID_API__ >= 9
  explicit OfflineRecognizerTransducerImpl(
      AAssetManager *mgr, const OfflineRecognizerConfig &config)
      : OfflineRecognizerImpl(mgr, config),
        config_(config),
        symbol_table_(mgr, config_.model_config.tokens),
        model_(std::make_unique<OfflineTransducerModel>(mgr,
                                                        config_.model_config)) {
    if (symbol_table_.Contains("<unk>")) {
      unk_id_ = symbol_table_["<unk>"];
    }

    if (config_.decoding_method == "greedy_search") {
      decoder_ = std::make_unique<OfflineTransducerGreedySearchDecoder>(
          model_.get(), unk_id_, config_.blank_penalty);
    } else if (config_.decoding_method == "modified_beam_search") {
      if (!config_.lm_config.model.empty()) {
        lm_ = OfflineLM::Create(mgr, config.lm_config);
      }

      if (!config_.model_config.bpe_vocab.empty()) {
        auto buf = ReadFile(mgr, config_.model_config.bpe_vocab);
        std::istringstream iss(std::string(buf.begin(), buf.end()));
        bpe_encoder_ = std::make_unique<ssentencepiece::Ssentencepiece>(iss);
      }

      if (!config_.hotwords_file.empty()) {
        InitHotwords(mgr);
      }

      decoder_ = std::make_unique<OfflineTransducerModifiedBeamSearchDecoder>(
          model_.get(), lm_.get(), config_.max_active_paths,
          config_.lm_config.scale, unk_id_, config_.blank_penalty);
    } else {
      SHERPA_ONNX_LOGE("Unsupported decoding method: %s",
                       config_.decoding_method.c_str());
      exit(-1);
    }
  }
#endif

  std::unique_ptr<OfflineStream> CreateStream(
      const std::string &hotwords) const override {
    auto hws = std::regex_replace(hotwords, std::regex("/"), "\n");
    std::istringstream is(hws);
    std::vector<std::vector<int32_t>> current;
    std::vector<float> current_scores;
    if (!EncodeHotwords(is, config_.model_config.modeling_unit, symbol_table_,
                        bpe_encoder_.get(), &current, &current_scores)) {
      SHERPA_ONNX_LOGE("Encode hotwords failed, skipping, hotwords are : %s",
                       hotwords.c_str());
    }

    int32_t num_default_hws = hotwords_.size();
    int32_t num_hws = current.size();

    current.insert(current.end(), hotwords_.begin(), hotwords_.end());

    if (!current_scores.empty() && !boost_scores_.empty()) {
      current_scores.insert(current_scores.end(), boost_scores_.begin(),
                            boost_scores_.end());
    } else if (!current_scores.empty() && boost_scores_.empty()) {
      current_scores.insert(current_scores.end(), num_default_hws,
                            config_.hotwords_score);
    } else if (current_scores.empty() && !boost_scores_.empty()) {
      current_scores.insert(current_scores.end(), num_hws,
                            config_.hotwords_score);
      current_scores.insert(current_scores.end(), boost_scores_.begin(),
                            boost_scores_.end());
    } else {
      // Do nothing.
    }

    auto context_graph = std::make_shared<ContextGraph>(
        current, config_.hotwords_score, current_scores);
    return std::make_unique<OfflineStream>(config_.feat_config, context_graph);
  }

  std::unique_ptr<OfflineStream> CreateStream() const override {
    return std::make_unique<OfflineStream>(config_.feat_config,
                                           hotwords_graph_);
  }

  void DecodeStreams(OfflineStream **ss, int32_t n) const override {
    using Clock = std::chrono::high_resolution_clock;  // 型エイリアスを定義
    auto start = Clock::now();

    auto memory_info =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    int32_t feat_dim = ss[0]->FeatureDim();

    auto start_feature_extraction = Clock::now();

    std::vector<Ort::Value> features;

    features.reserve(n);

    std::vector<std::vector<float>> features_vec(n);
    std::vector<int64_t> features_length_vec(n);
    for (int32_t i = 0; i != n; ++i) {
      auto f = ss[i]->GetFrames();
      int32_t num_frames = f.size() / feat_dim;

      features_length_vec[i] = num_frames;
      features_vec[i] = std::move(f);

      std::array<int64_t, 2> shape = {num_frames, feat_dim};

      Ort::Value x = Ort::Value::CreateTensor(
          memory_info, features_vec[i].data(), features_vec[i].size(),
          shape.data(), shape.size());
      features.push_back(std::move(x));
    }
    
    auto end_feature_extraction = Clock::now();
    std::chrono::duration<double> feature_extraction_time = end_feature_extraction - start_feature_extraction;
    std::cout << "Feature extraction time: " << feature_extraction_time.count() << " seconds." << std::endl;

    auto start_padding = Clock::now();

    std::vector<const Ort::Value *> features_pointer(n);
    for (int32_t i = 0; i != n; ++i) {
      features_pointer[i] = &features[i];
    }

    std::array<int64_t, 1> features_length_shape = {n};
    Ort::Value x_length = Ort::Value::CreateTensor(
        memory_info, features_length_vec.data(), n,
        features_length_shape.data(), features_length_shape.size());

    Ort::Value x = PadSequence(model_->Allocator(), features_pointer,
                               -23.025850929940457f);

    auto end_padding = Clock::now();
    std::chrono::duration<double> padding_time = end_padding - start_padding;
    std::cout << "Padding time: " << padding_time.count() << " seconds." << std::endl;

    auto start_encoder = Clock::now();

    auto t = model_->RunEncoder(std::move(x), std::move(x_length));

    auto end_encoder = Clock::now();
    std::chrono::duration<double> encoder_time = end_encoder - start_encoder;
    std::cout << "RunEncoder time: " << encoder_time.count() << " seconds." << std::endl;

    auto start_decoder = Clock::now();

    auto results =
        decoder_->Decode(std::move(t.first), std::move(t.second), ss, n);

    auto end_decoder = Clock::now();
    std::chrono::duration<double> decoder_time = end_decoder - start_decoder;
    std::cout << "Decoder time: " << decoder_time.count() << " seconds." << std::endl;

    int32_t frame_shift_ms = 10;
    for (int32_t i = 0; i != n; ++i) {
      auto r = Convert(results[i], symbol_table_, frame_shift_ms,
                       model_->SubsamplingFactor());
      r.text = ApplyInverseTextNormalization(std::move(r.text));

      ss[i]->SetResult(r);
    }

    auto end = Clock::now();
    std::chrono::duration<double> total_time = end - start;
    std::cout << "Total DecodeStreams time: " << total_time.count() << " seconds." << std::endl;
  }

  OfflineRecognizerConfig GetConfig() const override {
    return config_;
  }


  void InitHotwords() {
    // each line in hotwords_file contains space-separated words

    std::ifstream is(config_.hotwords_file);
    if (!is) {
      SHERPA_ONNX_LOGE("Open hotwords file failed: %s",
                       config_.hotwords_file.c_str());
      exit(-1);
    }

    if (!EncodeHotwords(is, config_.model_config.modeling_unit, symbol_table_,
                        bpe_encoder_.get(), &hotwords_, &boost_scores_)) {
      SHERPA_ONNX_LOGE(
          "Failed to encode some hotwords, skip them already, see logs above "
          "for details.");
    }
    hotwords_graph_ = std::make_shared<ContextGraph>(
        hotwords_, config_.hotwords_score, boost_scores_);
  }

#if __ANDROID_API__ >= 9
  void InitHotwords(AAssetManager *mgr) {
    // each line in hotwords_file contains space-separated words

    auto buf = ReadFile(mgr, config_.hotwords_file);

    std::istringstream is(std::string(buf.begin(), buf.end()));

    if (!is) {
      SHERPA_ONNX_LOGE("Open hotwords file failed: %s",
                       config_.hotwords_file.c_str());
      exit(-1);
    }

    if (!EncodeHotwords(is, config_.model_config.modeling_unit, symbol_table_,
                        bpe_encoder_.get(), &hotwords_, &boost_scores_)) {
      SHERPA_ONNX_LOGE(
          "Failed to encode some hotwords, skip them already, see logs above "
          "for details.");
    }
    hotwords_graph_ = std::make_shared<ContextGraph>(
        hotwords_, config_.hotwords_score, boost_scores_);
  }
#endif

 private:
  OfflineRecognizerConfig config_;
  SymbolTable symbol_table_;
  std::vector<std::vector<int32_t>> hotwords_;
  std::vector<float> boost_scores_;
  ContextGraphPtr hotwords_graph_;
  std::unique_ptr<ssentencepiece::Ssentencepiece> bpe_encoder_;
  std::unique_ptr<OfflineTransducerModel> model_;
  std::unique_ptr<OfflineTransducerDecoder> decoder_;
  std::unique_ptr<OfflineLM> lm_;
  int32_t unk_id_ = -1;
};

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_OFFLINE_RECOGNIZER_TRANSDUCER_IMPL_H_
