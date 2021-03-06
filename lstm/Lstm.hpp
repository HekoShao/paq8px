#ifndef PAQ8PX_LSTM_HPP
#define PAQ8PX_LSTM_HPP

#include "LstmLayer.hpp"
#include "SimdFunctions.hpp"
#include "Posit.hpp"
#include "../file/BitFileDisk.hpp"
#include "../file/OpenFromMyFolder.hpp"
#include "../utils.hpp"
#include <vector>
#include <memory>

/**
 * Long Short-Term Memory neural network.
 * Based on the LSTM implementation in cmix by Byron Knoll.
 */
template <SIMD simd, typename T>
class Lstm {
  static_assert(std::is_integral<T>::value && (!std::is_same<T, bool>::value), "LSTM input type must be integral and non-boolean");
private:
  std::vector<std::unique_ptr<LstmLayer<simd, T>>> layers;
  std::valarray<std::valarray<std::valarray<float>>> layer_input, output_layer;
  std::valarray<std::valarray<float>> output;
  std::valarray<float> hidden, hidden_error;
  std::vector<T> input_history;
  std::uint64_t saved_timestep;
  float learning_rate;
  std::size_t num_cells, horizon, input_size, output_size;

#if (defined(__GNUC__) || defined(__clang__)) && (!defined(__ARM_FEATURE_SIMD32) && !defined(__ARM_NEON))
  __attribute__((target("avx2,fma")))
#endif
  void SoftMaxSimdAVX2() {
#if !defined(__i386__) && !defined(__x86_64__) && !defined(_M_X64)
    return;
#else
    static constexpr std::size_t SIMDW = 8;
    std::size_t const limit = output_size & static_cast<std::size_t>(-static_cast<std::ptrdiff_t>(SIMDW)), len = hidden.size();
    std::size_t remainder = output_size & (SIMDW - 1);
    __m256 v_sum = _mm256_setzero_ps();
    for (std::size_t i = 0; i < limit; i++)
      output[epoch][i] = dot256_ps_fma3(&hidden[0], &output_layer[epoch][i][0], len, 0.f);
    for (std::size_t i = 0; i < limit; i += SIMDW) {
      __m256 v_exp = exp256_ps_fma3(_mm256_loadu_ps(&output[epoch][i]));
      _mm256_storeu_ps(&output[epoch][i], v_exp);
      v_sum = _mm256_add_ps(v_sum, v_exp);
    }
    float sum = hsum256_ps_avx(v_sum);
    for (; remainder > 0; remainder--) {
      const std::size_t i = output_size - remainder;
      output[epoch][i] = expa(dot256_ps_fma3(&hidden[0], &output_layer[epoch][i][0], len, 0.f));
      sum += output[epoch][i];
    }
    output[epoch] /= sum;
#endif
  }
  void SoftMaxSimdNone() {
    for (unsigned int i = 0; i < output_size; ++i)
      output[epoch][i] = expa(SumOfProducts(&hidden[0], &output_layer[epoch][i][0], hidden.size()));
    float s = 0.0f;
    for (int i = 0; i < output[epoch].size(); i++) s += output[epoch][i];
    for (int i = 0; i < output[epoch].size(); i++) output[epoch][i] /= s;
  }
public:
  std::size_t epoch;
  Lstm(
    std::size_t const input_size,
    std::size_t const output_size,
    std::size_t const num_cells,
    std::size_t const num_layers,
    std::size_t const horizon,
    float const learning_rate,
    float const gradient_clip) :
    layer_input(std::valarray<std::valarray<float>>(std::valarray<float>(input_size + 1 + num_cells * 2), num_layers), horizon),
    output_layer(std::valarray<std::valarray<float>>(std::valarray<float>(num_cells * num_layers + 1), output_size), horizon),
    output(std::valarray<float>(1.0f / output_size, output_size), horizon),
    hidden(num_cells * num_layers + 1),
    hidden_error(num_cells),
    input_history(horizon),
    saved_timestep(0),
    learning_rate(learning_rate),
    num_cells(num_cells),
    horizon(horizon),
    input_size(input_size),
    output_size(output_size),
    epoch(0)
  {
    hidden[hidden.size() - 1] = 1.f;
    for (std::size_t epoch = 0; epoch < horizon; epoch++) {
      layer_input[epoch][0].resize(1 + num_cells + input_size);
      for (std::size_t i = 0; i < num_layers; i++)
        layer_input[epoch][i][layer_input[epoch][i].size() - 1] = 1.f;
    }
    for (std::size_t i = 0; i < num_layers; i++) {
      layers.push_back(std::unique_ptr<LstmLayer<simd, T>>(new LstmLayer<simd, T>(
        layer_input[0][i].size() + output_size,
        input_size, output_size,
        num_cells, horizon, gradient_clip, learning_rate
        )));
    }
  }

  void SetInput(std::valarray<float> const& input) {
    for (std::size_t i = 0; i < layers.size(); i++)
      memcpy(&layer_input[epoch][i][0], &input[0], input_size * sizeof(float));
  }

  std::valarray<float>& Predict(T const input) {
    for (std::size_t i = 0; i < layers.size(); i++) {
      memcpy(&layer_input[epoch][i][input_size], &hidden[i * num_cells], num_cells * sizeof(float));
      layers[i]->ForwardPass(layer_input[epoch][i], input, &hidden, i * num_cells);
      if (i < layers.size() - 1) {
        memcpy(&layer_input[epoch][i + 1][num_cells + input_size], &hidden[i * num_cells], num_cells * sizeof(float));
      }
    }
    if (simd == SIMD_AVX2)
      SoftMaxSimdAVX2();
    else
      SoftMaxSimdNone();
    std::size_t const epoch_ = epoch;
    epoch++;
    if (epoch == horizon) epoch = 0;
    return output[epoch_];
  }

  std::valarray<float>& Perceive(const T input) {
    std::size_t const last_epoch = ((epoch > 0) ? epoch : horizon) - 1;
    T const old_input = input_history[last_epoch];
    input_history[last_epoch] = input;
    if (epoch == 0) {
      for (int epoch_ = static_cast<int>(horizon) - 1; epoch_ >= 0; epoch_--) {
        for (int layer = static_cast<int>(layers.size()) - 1; layer >= 0; layer--) {
          int offset = layer * static_cast<int>(num_cells);
          for (std::size_t i = 0; i < output_size; i++) {
            float const error = (i == input_history[epoch_]) ? output[epoch_][i] - 1.f : output[epoch_][i];
            for (std::size_t j = 0; j < hidden_error.size(); j++)
              hidden_error[j] += output_layer[epoch_][i][j + offset] * error;
          }
          std::size_t const prev_epoch = ((epoch_ > 0) ? epoch_ : horizon) - 1;
          T const input_symbol = (epoch_ > 0) ? input_history[prev_epoch] : old_input;
          layers[layer]->BackwardPass(layer_input[epoch_][layer], epoch_, layer, input_symbol, &hidden_error);
        }
      }
    }
    for (std::size_t i = 0; i < output_size; i++) {
      float const error = (i == input) ? output[last_epoch][i] - 1.f : output[last_epoch][i];
      for (int j = 0; j < hidden.size(); j++) {
        output_layer[epoch][i][j] = output_layer[last_epoch][i][j]- learning_rate * error * hidden[j];
      }
    }
    return Predict(input);
  }

  void SaveTimeStep() {
    saved_timestep = layers[0]->update_steps;
  }

  void RestoreTimeStep() {
    for (std::size_t i = 0; i < layers.size(); i++)
      layers[i]->update_steps = saved_timestep;
  }

  template<std::int32_t bits = 0, std::int32_t exp = 0>
  void LoadFromDisk(const char* const dictionary);
  
  template<std::int32_t bits = 0, std::int32_t exp = 0>
  void SaveToDisk(const char* const dictionary);
};

template <SIMD simd, typename T>
template<std::int32_t bits, std::int32_t exp>
void Lstm<simd, T>::LoadFromDisk(const char* const dictionary) {
  static_assert((bits >= 0) && (bits <= 16), "LSTM::LoadFromDisk template parameter @bits must be in range [0..16]");
  std::size_t const last_epoch = ((epoch > 0) ? epoch : horizon) - 1;
  BitFileDisk file(true);
  OpenFromMyFolder::anotherFile(&file, dictionary);
  if ((bits > 0) && (bits <= 16)) {
    float scale = Posit<9, 1>::Decode(file.getBits(8u));
    for (std::size_t i = 0u; i < output_size; i++) {
      for (std::size_t j = 0u; j < output_layer[0][i].size(); j++)
        output_layer[last_epoch][i][j] = Posit<bits, exp>::Decode(file.getBits(bits)) * scale;
    }
    for (std::size_t i = 0u; i < layers.size(); i++) {
      auto weights = layers[i]->Weights();
      for (std::size_t j = 0u; j < weights.size(); j++) {
        for (std::size_t k = 0u; k < weights[j]->size(); k++) {
          for (std::size_t l = 0u; l < (*weights[j])[k].size(); l++)
            (*weights[j])[k][l] = Posit<bits, exp>::Decode(file.getBits(bits)) * scale;
        }
      }
    }
  }
  else {
    float v;
    for (std::size_t i = 0u; i < output_size; i++) {
      for (std::size_t j = 0u; j < output_layer[0][i].size(); j++) {
        if (file.blockRead(reinterpret_cast<std::uint8_t*>(&v), sizeof(float)) != sizeof(float)) break;
        output_layer[last_epoch][i][j] = v;
      }
    }
    for (std::size_t i = 0u; i < layers.size(); i++) {
      auto weights = layers[i]->Weights();
      for (std::size_t j = 0u; j < weights.size(); j++) {
        for (std::size_t k = 0u; k < weights[j]->size(); k++) {
          for (std::size_t l = 0u; l < (*weights[j])[k].size(); l++) {
            if (file.blockRead(reinterpret_cast<std::uint8_t*>(&v), sizeof(float)) != sizeof(float)) break;
            (*weights[j])[k][l] = v;
          }
        }
      }
    }
  }
  file.close();
}

template <SIMD simd, typename T>
template<std::int32_t bits, std::int32_t exp>
void Lstm<simd, T>::SaveToDisk(const char* const dictionary) {
  static_assert((bits >= 0) && (bits <= 16), "LSTM::SaveToDisk template parameter @bits must be in range [0..16]");  
  std::size_t const last_epoch = ((epoch > 0) ? epoch : horizon) - 1;
  BitFileDisk file(false);
  file.create(dictionary);
  if ((bits > 0) && (bits <= 16)) {
    std::uint32_t buf = 0u;
    std::int32_t constexpr buf_width = static_cast<std::int32_t>(sizeof(buf)) * 8;
    std::int32_t bits_left = buf_width;
    float const s = std::pow(2.f, (1 << exp) * (bits - 2));
    float max_w = 0.f, w, scale;
    for (std::size_t i = 0u; i < output_size; i++) {
      for (std::size_t j = 0u; j < output_layer[0][i].size(); j++) {
        if ((w = std::fabs(output_layer[last_epoch][i][j])) > max_w)
          max_w = w;
      }
    }
    for (std::size_t i = 0u; i < layers.size(); i++) {
      auto weights = layers[i]->Weights();
      for (std::size_t j = 0u; j < weights.size(); j++) {
        for (std::size_t k = 0u; k < weights[j]->size(); k++) {
          for (std::size_t l = 0u; l < (*weights[j])[k].size(); l++) {
            if ((w = std::fabs((*weights[j])[k][l])) > max_w)
              max_w = w;
          }
        }
      }
    }
    scale = Posit<9, 1>::Decode(Posit<9, 1>::Encode(std::max<float>(1.f, max_w / s)));
    file.putBits(Posit<9, 1>::Encode(scale), 8u);
    for (std::size_t i = 0u; i < output_size; i++) {
      for (std::size_t j = 0u; j < output_layer[0][i].size(); j++)
        file.putBits(Posit<bits, exp>::Encode(output_layer[last_epoch][i][j] / scale), bits);
    }
    for (std::size_t i = 0u; i < layers.size(); i++) {
      auto weights = layers[i]->Weights();
      for (std::size_t j = 0u; j < weights.size(); j++) {
        for (std::size_t k = 0u; k < weights[j]->size(); k++) {
          for (std::size_t l = 0u; l < (*weights[j])[k].size(); l++)
            file.putBits(Posit<bits, exp>::Encode((*weights[j])[k][l] / scale), bits);
        }
      }
    }
    file.flush();
  }
  else {
    float v;
    for (std::size_t i = 0u; i < output_size; i++) {
      for (std::size_t j = 0u; j < output_layer[0][i].size(); j++) {
        v = output_layer[last_epoch][i][j];
        file.blockWrite(reinterpret_cast<std::uint8_t*>(&v), sizeof(float));        
      }
    }
    for (std::size_t i = 0u; i < layers.size(); i++) {
      auto weights = layers[i]->Weights();
      for (std::size_t j = 0u; j < weights.size(); j++) {
        for (std::size_t k = 0u; k < weights[j]->size(); k++) {
          for (std::size_t l = 0u; l < (*weights[j])[k].size(); l++) {
            v = (*weights[j])[k][l];
            file.blockWrite(reinterpret_cast<std::uint8_t*>(&v), sizeof(float));
          }
        }
      }
    }
  }
  file.close();
}

#endif //PAQ8PX_LSTM_HPP
