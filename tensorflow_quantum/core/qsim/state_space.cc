/* Copyright 2020 The TensorFlow Quantum Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_quantum/core/qsim/state_space.h"

#include <complex>
#include <memory>

#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/random/simple_philox.h"
#include "tensorflow_quantum/core/proto/pauli_sum.pb.h"
#include "tensorflow_quantum/core/qsim/fuser_basic.h"
#include "tensorflow_quantum/core/src/circuit.h"
#include "tensorflow_quantum/core/src/circuit_parser.h"
#include "tensorflow_quantum/core/src/matrix.h"

namespace tfq {
namespace qsim {

tensorflow::Status StateSpace::Update(const Circuit& circuit) {
  tensorflow::Status status;
  // Special case for single qubit;
  // derived classes free to return an error.
  if (GetDimension() <= 2) {
    for (uint64_t i = 0; i < circuit.gates.size(); i++) {
      const auto& gate = circuit.gates[i];
      if (gate.num_qubits == 1) {
        float matrix[8];
        Matrix2Set(gate.matrix, matrix);
        status = ApplyGate1(matrix);
        if (!status.ok()) {
          return status;
        }
      } else {
        return tensorflow::Status(
            tensorflow::error::INVALID_ARGUMENT,
            "Got a multi-qubit gate in a 1 qubit circuit.");
      }
    }
    return tensorflow::Status::OK();
  }

  std::vector<GateFused> fused_gates;
  status = FuseGates(circuit, &fused_gates);
  if (!status.ok()) {
    return status;
  }

  for (const GateFused& gate : fused_gates) {
    float matrix[32];
    CalcMatrix4(gate.qubits[0], gate.qubits[1], gate.gates, matrix);
    ApplyGate2(gate.qubits[0], gate.qubits[1], matrix);
  }

  return tensorflow::Status::OK();
}

tensorflow::Status StateSpace::ComputeExpectation(
    const tfq::proto::PauliSum& p_sum, StateSpace* scratch,
    float* expectation_value) {
  // apply the  gates of the pauliterms to a copy of the wavefunction
  // and add up expectation value term by term.
  tensorflow::Status status = tensorflow::Status::OK();
  for (const tfq::proto::PauliTerm& term : p_sum.terms()) {
    // catch identity terms
    if (term.paulis_size() == 0) {
      *expectation_value += term.coefficient_real();
      // TODO(zaqqwerty): error somewhere if identities have any imaginary part
      continue;
    }

    Circuit measurement_circuit;

    status = CircuitFromPauliTerm(term, num_qubits_, &measurement_circuit);
    if (!status.ok()) {
      return status;
    }
    scratch->CopyFrom(*this);
    status = scratch->Update(measurement_circuit);
    if (!status.ok()) {
      return status;
    }
    *expectation_value +=
        term.coefficient_real() * GetRealInnerProduct(*scratch);
  }
  return status;
}

void StateSpace::SampleState(const int m, std::vector<uint64_t>* samples) {
  // An alternate would be to use:
  // tensorflow/core/lib/random/distribution_sampler.h which would have a
  // runtime of:
  // O(2 ** num_qubits + m) and additional mem O(2 ** num_qubits + m)
  // This method has (which is good because memory is expensive to get):
  // O(2 ** num_qubits + m * log(m)) and additional mem O(m)
  // Note: random samples in samples will appear in order.
  if (m == 0) {
    return;
  }
  tensorflow::random::PhiloxRandom philox(std::rand());
  tensorflow::random::SimplePhilox gen(&philox);

  double cdf_so_far = 0.0;
  std::vector<float> random_vals(m, 0.0);
  samples->reserve(m);
  for (int i = 0; i < m; i++) {
    random_vals[i] = gen.RandFloat();
  }
  std::sort(random_vals.begin(), random_vals.end());

  int j = 0;
  for (uint64_t i = 0; i < GetDimension(); i++) {
    const std::complex<float> f_amp = GetAmpl(i);
    const std::complex<double> d_amp = std::complex<double>(
        static_cast<double>(f_amp.real()), static_cast<double>(f_amp.imag()));
    cdf_so_far += std::norm(d_amp);
    while (random_vals[j] < cdf_so_far && j < m) {
      samples->push_back(i);
      j++;
    }
  }

  // Safety measure in case of state norm underflow.
  // Likely to not have huge impact.
  while (j < m) {
    samples->push_back(samples->at(samples->size() - 1));
    j++;
  }
}

bool StateSpace::Valid() const {
  // TODO: more roubust test?
  return state_ != nullptr;
}

float* StateSpace::GetRawState() const { return state_; };

uint64_t StateSpace::GetDimension() const { return size_ / 2; }

uint64_t StateSpace::GetNumQubits() const { return num_qubits_; }

uint64_t StateSpace::GetNumThreads() const { return num_threads_; }

}  // namespace qsim
}  // namespace tfq
