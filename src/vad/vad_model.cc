#include "vad/vad_model.h"

#include <cmath>
#include <cstring>

#include "glog/logging.h"

VadModel::VadModel(const std::string& model_path, bool denoise, int sample_rate,
                   float threshold, int min_sil_dur_ms, int speech_pad_ms)
    : OnnxModel(model_path),
      denoise_(denoise),
      sample_rate_(sample_rate),
      threshold_(threshold),
      min_sil_dur_samples_(min_sil_dur_ms * sample_rate / 1000),
      speech_pad_samples_(speech_pad_ms * sample_rate / 1000) {
  denoiser_ = std::make_shared<Denoiser>();
  resampler_ = std::make_shared<Resampler>();
  sample_queue_ = std::make_shared<SampleQueue>();
  Reset();
}

void VadModel::Reset() {
  h_.resize(SIZE_HC);
  c_.resize(SIZE_HC);
  std::memset(h_.data(), 0.0f, SIZE_HC * sizeof(float));
  std::memset(c_.data(), 0.0f, SIZE_HC * sizeof(float));
  on_speech_ = false;
  temp_end_ = 0;
  current_sample_ = 0;
  sample_queue_->Clear();
  denoiser_->Reset();
}

float VadModel::Forward(const std::vector<float>& pcm) {
  std::vector<float> input_pcm{pcm.data(), pcm.data() + pcm.size()};
  for (int i = 0; i < input_pcm.size(); i++) {
    input_pcm[i] /= 32768.0;
  }

  // batch_size * num_samples
  const int64_t batch_size = 1;
  int64_t input_node_dims[2] = {batch_size, input_pcm.size()};
  auto input_ort = Ort::Value::CreateTensor<float>(
      memory_info_, input_pcm.data(), input_pcm.size(), input_node_dims, 2);

  const int64_t sr_node_dims[1] = {batch_size};
  std::vector<int64_t> sr = {sample_rate_};
  auto sr_ort = Ort::Value::CreateTensor<int64_t>(memory_info_, sr.data(),
                                                  batch_size, sr_node_dims, 1);
  const int64_t hc_node_dims[3] = {2, batch_size, 64};
  auto h_ort = Ort::Value::CreateTensor<float>(memory_info_, h_.data(), SIZE_HC,
                                               hc_node_dims, 3);
  auto c_ort = Ort::Value::CreateTensor<float>(memory_info_, c_.data(), SIZE_HC,
                                               hc_node_dims, 3);

  std::vector<Ort::Value> ort_inputs;
  ort_inputs.emplace_back(std::move(input_ort));
  ort_inputs.emplace_back(std::move(sr_ort));
  ort_inputs.emplace_back(std::move(h_ort));
  ort_inputs.emplace_back(std::move(c_ort));

  auto ort_outputs = session_->Run(
      Ort::RunOptions{nullptr}, input_node_names_.data(), ort_inputs.data(),
      ort_inputs.size(), output_node_names_.data(), output_node_names_.size());

  float posterier = ort_outputs[0].GetTensorMutableData<float>()[0];
  float* hn = ort_outputs[1].GetTensorMutableData<float>();
  float* cn = ort_outputs[2].GetTensorMutableData<float>();
  h_.assign(hn, hn + SIZE_HC);
  c_.assign(cn, cn + SIZE_HC);

  return posterier;
}

void VadModel::AcceptWaveform(const std::vector<float>& pcm) {
  std::vector<float> in_pcm{pcm.data(), pcm.data() + pcm.size()};
  if (denoise_) {
    std::vector<float> resampled_pcm;
    std::vector<float> denoised_pcm;
    // 0. Upsample to 48k for RnNoise
    if (sample_rate_ != 48000) {
      resampler_->Resample(sample_rate_, in_pcm, 48000, &resampled_pcm);
      in_pcm = resampled_pcm;
    }
    // 1. Denoise with RnNoise
    denoiser_->Denoise(in_pcm, &denoised_pcm);
    in_pcm = denoised_pcm;
    // 2. Downsample to 16k for VAD
    resampler_->Resample(48000, in_pcm, 16000, &resampled_pcm);
    sample_rate_ = 16000;
    in_pcm = resampled_pcm;
  }
  sample_queue_->AcceptWaveform(in_pcm);
}

void VadModel::Vad(const std::vector<float>& pcm, std::vector<float>* start_pos,
                   std::vector<float>* end_pos) {
  AcceptWaveform(pcm);

  std::vector<float> in_pcm;
  int num_frames = sample_queue_->NumSamples() / frame_size_;
  for (int i = 0; i < num_frames; i++) {
    sample_queue_->Read(frame_size_, &in_pcm);
    int window_size_samples = in_pcm.size();
    current_sample_ += window_size_samples;
    float speech_prob = Forward(in_pcm);

    // 1. start
    if (speech_prob >= threshold_) {
      temp_end_ = 0;
      if (on_speech_ == false) {
        on_speech_ = true;
        float speech_start =
            current_sample_ - window_size_samples - speech_pad_samples_;
        start_pos->emplace_back(round(speech_start / sample_rate_ * 1000) /
                                1000);
      }
    }
    // 2. stop
    if (speech_prob < (threshold_ - 0.15) && on_speech_ == true) {
      if (temp_end_ == 0) {
        temp_end_ = current_sample_;
      }
      // hangover
      if (current_sample_ - temp_end_ >= min_sil_dur_samples_) {
        float speech_end = current_sample_ + speech_pad_samples_;
        temp_end_ = 0;
        on_speech_ = false;
        end_pos->emplace_back(round(speech_end / sample_rate_ * 1000) / 1000);
      }
    }
  }
}
