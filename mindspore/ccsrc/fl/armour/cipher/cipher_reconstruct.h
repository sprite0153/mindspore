/**
 * Copyright 2021 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MINDSPORE_CCSRC_ARMOUR_CIPHER_RECONSTRUCT_H
#define MINDSPORE_CCSRC_ARMOUR_CIPHER_RECONSTRUCT_H

#include <vector>
#include <string>
#include <memory>
#include <map>
#include <utility>
#include "fl/armour/secure_protocol/secret_sharing.h"
#include "proto/ps.pb.h"
#include "utils/log_adapter.h"
#include "fl/armour/cipher/cipher_init.h"
#include "fl/armour/cipher/cipher_meta_storage.h"

namespace mindspore {
namespace armour {
// The process of reconstruct secret mask in the secure aggregation
class CipherReconStruct {
 public:
  // initialize: get cipher_init_
  CipherReconStruct() { cipher_init_ = &CipherInit::GetInstance(); }

  static CipherReconStruct &GetInstance() {
    static CipherReconStruct instance;
    return instance;
  }

  // reconstruct secret mask
  bool ReconstructSecrets(const int cur_iterator, const std::string &next_req_time,
                          const schema::SendReconstructSecret *reconstruct_secret_req,
                          std::shared_ptr<fl::server::FBBuilder> reconstruct_secret_resp_builder,
                          const std::vector<std::string> &client_list);

  // build response code of reconstruct secret.
  void BuildReconstructSecretsRsp(std::shared_ptr<fl::server::FBBuilder> fbb, const schema::ResponseCode retcode,
                                  const std::string &reason, const int iteration, const std::string &next_req_time);

  // clear the shared memory.
  void ClearReconstructSecrets();

 private:
  CipherInit *cipher_init_;  // the parameter of the secure aggregation
  // get mask symbol by comparing str1 and str2.
  bool GetSymbol(const std::string &str1, const std::string &str2);
  // get suv noise by computing shares result.
  bool GetSuvNoise(const std::vector<std::string> &clients_share_list,
                   const std::map<std::string, std::vector<std::vector<unsigned char>>> &record_public_keys,
                   const string &fl_id, std::vector<float> *noise, char *secret, int length);
  // malloc shares.
  bool MallocShares(std::vector<Share *> *shares_tmp, int shares_size);
  // delete shares.
  void DeleteShares(std::vector<Share *> *shares_tmp);
  // convert shares from receiving clients to sending clients.
  void ConvertSharesToShares(const std::map<std::string, std::vector<clientshare_str>> &src,
                             std::map<std::string, std::vector<clientshare_str>> *des);
  // generate noise from shares.
  bool ReconstructSecretsGenNoise(const std::vector<string> &client_list);
  // get noise masks sum.
  bool GetNoiseMasksSum(std::vector<float> *result, const std::map<std::string, std::vector<float>> &client_keys);

  // combine noise mask.
  bool CombineMask(std::vector<Share *> *shares_tmp, std::map<std::string, std::vector<float>> *client_keys,
                   const std::vector<std::string> &clients_share_list,
                   const std::map<std::string, std::vector<std::vector<unsigned char>>> &record_public_keys,
                   const std::map<std::string, std::vector<clientshare_str>> &reconstruct_secret_list,
                   const std::vector<string> &client_list);
};
}  // namespace armour
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_ARMOUR_CIPHER_KEYS_H
