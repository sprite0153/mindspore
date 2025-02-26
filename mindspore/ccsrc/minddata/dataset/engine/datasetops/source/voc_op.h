/**
 * Copyright 2019-2021 Huawei Technologies Co., Ltd
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
#ifndef MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_DATASETOPS_SOURCE_VOC_OP_H_
#define MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_DATASETOPS_SOURCE_VOC_OP_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "./tinyxml2.h"
#include "minddata/dataset/core/tensor.h"

#include "minddata/dataset/engine/data_schema.h"
#include "minddata/dataset/engine/datasetops/parallel_op.h"
#include "minddata/dataset/engine/datasetops/source/mappable_leaf_op.h"
#include "minddata/dataset/engine/datasetops/source/sampler/sampler.h"
#include "minddata/dataset/kernels/image/image_utils.h"
#include "minddata/dataset/util/path.h"
#include "minddata/dataset/util/queue.h"
#include "minddata/dataset/util/status.h"
#include "minddata/dataset/util/wait_post.h"

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;
using tinyxml2::XMLError;
namespace mindspore {
namespace dataset {
// Forward declares
template <typename T>
class Queue;

using Annotation = std::vector<std::pair<std::string, std::vector<float>>>;

class VOCOp : public MappableLeafOp {
 public:
  enum class TaskType { Segmentation = 0, Detection = 1 };

  class Builder {
   public:
    // Constructor for Builder class of ImageFolderOp
    // @param  uint32_t numWrks - number of parallel workers
    // @param dir - directory folder got ImageNetFolder
    Builder();

    // Destructor.
    ~Builder() = default;

    // Setter method.
    // @param const std::string & build_dir
    // @return Builder setter method returns reference to the builder.
    Builder &SetDir(const std::string &build_dir) {
      builder_dir_ = build_dir;
      return *this;
    }

    // Setter method.
    // @param const std::map<std::string, int32_t> &map - a class name to label map
    // @return Builder setter method returns reference to the builder.
    Builder &SetClassIndex(const std::map<std::string, int32_t> &map) {
      builder_labels_to_read_ = map;
      return *this;
    }

    // Setter method.
    // @param const std::string &task_type
    // @return Builder setter method returns reference to the builder.
    Builder &SetTask(const std::string &task_type) {
      if (task_type == "Segmentation") {
        builder_task_type_ = TaskType::Segmentation;
      } else if (task_type == "Detection") {
        builder_task_type_ = TaskType::Detection;
      }
      return *this;
    }

    // Setter method.
    // @param const std::string &usage
    // @return Builder setter method returns reference to the builder.
    Builder &SetUsage(const std::string &usage) {
      builder_usage_ = usage;
      return *this;
    }

    // Setter method.
    // @param int32_t num_workers
    // @return Builder setter method returns reference to the builder.
    Builder &SetNumWorkers(int32_t num_workers) {
      builder_num_workers_ = num_workers;
      return *this;
    }

    // Setter method.
    // @param int32_t op_connector_size
    // @return Builder setter method returns reference to the builder.
    Builder &SetOpConnectorSize(int32_t op_connector_size) {
      builder_op_connector_size_ = op_connector_size;
      return *this;
    }

    // Setter method.
    // @param std::shared_ptr<Sampler> sampler
    // @return Builder setter method returns reference to the builder.
    Builder &SetSampler(std::shared_ptr<SamplerRT> sampler) {
      builder_sampler_ = std::move(sampler);
      return *this;
    }

    // Setter method.
    // @param bool do_decode
    // @return Builder setter method returns reference to the builder.
    Builder &SetDecode(bool do_decode) {
      builder_decode_ = do_decode;
      return *this;
    }

    // Check validity of input args
    // @return Status The status code returned
    Status SanityCheck();

    // The builder "Build" method creates the final object.
    // @param std::shared_ptr<VOCOp> *op - DatasetOp
    // @return Status The status code returned
    Status Build(std::shared_ptr<VOCOp> *op);

   private:
    bool builder_decode_;
    std::string builder_dir_;
    TaskType builder_task_type_;
    std::string builder_usage_;
    int32_t builder_num_workers_;
    int32_t builder_op_connector_size_;
    int32_t builder_rows_per_buffer_;
    std::shared_ptr<SamplerRT> builder_sampler_;
    std::unique_ptr<DataSchema> builder_schema_;
    std::map<std::string, int32_t> builder_labels_to_read_;
  };

  // Constructor
  // @param TaskType task_type - task type of VOC
  // @param std::string task_mode - task mode of VOC
  // @param std::string folder_path - dir directory of VOC
  // @param std::map<std::string, int32_t> class_index - input class-to-index of annotation
  // @param int32_t num_workers - number of workers reading images in parallel
  // @param int32_t queue_size - connector queue size
  // @param bool decode - whether to decode images
  // @param std::unique_ptr<DataSchema> data_schema - the schema of the VOC dataset
  // @param std::shared_ptr<Sampler> sampler - sampler tells VOCOp what to read
  // @param extra_metadata - flag to add extra meta-data to row
  VOCOp(const TaskType &task_type, const std::string &task_mode, const std::string &folder_path,
        const std::map<std::string, int32_t> &class_index, int32_t num_workers, int32_t queue_size, bool decode,
        std::unique_ptr<DataSchema> data_schema, std::shared_ptr<SamplerRT> sampler, bool extra_metadata);

  // Destructor
  ~VOCOp() = default;

  // A print method typically used for debugging
  // @param out
  // @param show_all
  void Print(std::ostream &out, bool show_all) const override;

  // @param const std::string &dir - VOC dir path
  // @param const std::string &task_type - task type of reading voc job
  // @param const std::string &task_mode - task mode of reading voc job
  // @param const std::map<std::string, int32_t> input_class_indexing - input map of class index
  // @param int64_t *count - output rows number of VOCDataset
  static Status CountTotalRows(const std::string &dir, const std::string &task_type, const std::string &task_mode,
                               const std::map<std::string, int32_t> &input_class_indexing, int64_t *count);

#ifdef ENABLE_PYTHON
  // @param const std::string &dir - VOC dir path
  // @param const std::string &task_type - task type of reading voc job
  // @param const std::string &task_mode - task mode of reading voc job
  // @param const py::dict &dict - input dict of class index
  // @param int64_t numSamples - samples number of VOCDataset
  // @param std::map<std::string, int32_t> *output_class_indexing - output class index of VOCDataset
  static Status GetClassIndexing(const std::string &dir, const std::string &task_type, const std::string &task_mode,
                                 const py::dict &dict, std::map<std::string, int32_t> *output_class_indexing);
#endif

  // Op name getter
  // @return Name of the current Op
  std::string Name() const override { return "VOCOp"; }

  // /// \brief Gets the class indexing
  // /// \return Status - The status code return
  Status GetClassIndexing(std::vector<std::pair<std::string, std::vector<int32_t>>> *output_class_indexing) override;

 private:
  // Load a tensor row according to image id
  // @param row_id_type row_id - id for this tensor row
  // @param std::string image_id - image id
  // @param TensorRow row - image & target read into this tensor row
  // @return Status The status code returned
  Status LoadTensorRow(row_id_type row_id, TensorRow *row) override;

  // @param const std::string &path - path to the image file
  // @param const ColDescriptor &col - contains tensor implementation and datatype
  // @param std::shared_ptr<Tensor> tensor - return
  // @return Status The status code returned
  Status ReadImageToTensor(const std::string &path, const ColDescriptor &col, std::shared_ptr<Tensor> *tensor);

  // @param const std::string &path - path to the image file
  // @param TensorRow *row - return
  // @return Status The status code returned
  Status ReadAnnotationToTensor(const std::string &path, TensorRow *row);

  // Read image list from ImageSets
  // @return Status The status code returned
  Status ParseImageIds();

  // Read annotation from Annotation folder
  // @return Status The status code returned
  Status ParseAnnotationIds();

  // @param const std::string &path - path to annotation xml
  // @return Status The status code returned
  Status ParseAnnotationBbox(const std::string &path);

  // @param xmin - the left coordinate of bndbox
  // @param ymin - the top coordinate of bndbox
  // @param xmax - the right coordinate of bndbox
  // @param ymax - the bottom coordinate of bndbox
  // @param path - the file path of bndbox xml
  // @return Status The status code returned
  Status CheckIfBboxValid(const float &xmin, const float &ymin, const float &xmax, const float &ymax,
                          const std::string &path);

  // @param XMLElement *bbox_node - bbox node info found in json object
  // @param const char *name - sub node name in object
  // @param float *value - value of certain sub node
  // @return Status The status code returned
  void ParseNodeValue(XMLElement *bbox_node, const char *name, float *value);

  // Called first when function is called
  // @return Status The status code returned
  Status LaunchThreadsAndInitOp() override;

  // Private function for computing the assignment of the column name map.
  // @return - Status
  Status ComputeColMap() override;

  bool decode_;
  int64_t row_cnt_;
  std::string folder_path_;
  TaskType task_type_;
  std::string usage_;
  std::unique_ptr<DataSchema> data_schema_;
  bool extra_metadata_;

  std::vector<std::string> image_ids_;
  std::map<std::string, int32_t> class_index_;
  std::map<std::string, int32_t> label_index_;
  std::map<std::string, Annotation> annotation_map_;
};
}  // namespace dataset
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_DATASETOPS_SOURCE_VOC_OP_H_
