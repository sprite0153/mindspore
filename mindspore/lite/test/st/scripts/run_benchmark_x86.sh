#!/bin/bash
source ./scripts/base_functions.sh

# Run converter on x86 platform:
function Run_Converter() {
    # Unzip x86 runtime and converter
    cd ${x86_path} || exit 1
    tar -zxf mindspore-lite-${version}-linux-x64.tar.gz || exit 1
    cd ${x86_path}/mindspore-lite-${version}-linux-x64/ || exit 1

    cp tools/converter/converter/converter_lite ./ || exit 1
    export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:./tools/converter/lib/:./tools/converter/third_party/glog/lib

    rm -rf ${ms_models_path}
    mkdir -p ${ms_models_path}

    # Convert TFLite parallel_split models:
    while read line; do
        parallel_split_line_info=${line}
        if [[ $parallel_split_line_info == \#* ]]; then
          continue
        fi
        model_name=`echo ${parallel_split_line_info}|awk -F ' ' '{print $1}'`
        echo ${model_name} >> "${run_converter_log_file}"
        echo 'convert mode name: '${model_name}' begin.'
        echo './converter_lite  --fmk=TFLITE --modelFile='${models_path}'/'${model_name}' --outputFile='${ms_models_path}'/'${model_name}_1_1_parallel_split' --config_file='${models_path}'/offline_parallel_split/1_1_parallel_split.config' >> "${run_converter_log_file}"
        ./converter_lite  --fmk=TFLITE --modelFile=$models_path/${model_name} --outputFile=${ms_models_path}/${model_name}_1_1_parallel_split --configFile=${models_path}/offline_parallel_split/1_1_parallel_split.config
        if [ $? = 0 ]; then
            converter_result='converter 1_1_parallel_split '${model_name}' pass';echo ${converter_result} >> ${run_converter_result_file}
        else
            converter_result='converter 1_1_parallel_split '${model_name}' failed';echo ${converter_result} >> ${run_converter_result_file};return 1
        fi

        echo './converter_lite  --fmk=TFLITE --modelFile='${models_path}'/'${model_name}' --outputFile='${ms_models_path}'/'${model_name}_1_2_parallel_split' --config_file='${models_path}'/offline_parallel_split/1_2_parallel_split.config' >> "${run_converter_log_file}"
        ./converter_lite  --fmk=TFLITE --modelFile=$models_path/${model_name} --outputFile=${ms_models_path}/${model_name}_1_2_parallel_split --configFile=${models_path}/offline_parallel_split/1_2_parallel_split.config
        if [ $? = 0 ]; then
            converter_result='converter 1_2_parallel_split '${model_name}' pass';echo ${converter_result} >> ${run_converter_result_file}
        else
            converter_result='converter 1_2_parallel_split '${model_name}' failed';echo ${converter_result} >> ${run_converter_result_file};return 1
        fi

        echo './converter_lite  --fmk=TFLITE --modelFile='${models_path}'/'${model_name}' --outputFile='${ms_models_path}'/'${model_name}_1_3_parallel_split' --config_file='${models_path}'/offline_parallel_split/1_3_parallel_split.config' >> "${run_converter_log_file}"
        ./converter_lite  --fmk=TFLITE --modelFile=$models_path/${model_name} --outputFile=${ms_models_path}/${model_name}_1_3_parallel_split --configFile=${models_path}/offline_parallel_split/1_3_parallel_split.config
        if [ $? = 0 ]; then
            converter_result='converter 1_3_parallel_split '${model_name}' pass';echo ${converter_result} >> ${run_converter_result_file}
        else
            converter_result='converter 1_3_parallel_split '${model_name}' failed';echo ${converter_result} >> ${run_converter_result_file};return 1
        fi

    done < ${models_tflite_parallel_split_config}

    # Prepare the config file list
    local x86_cfg_file_list=("$models_tf_config" "$models_tflite_config" "$models_caffe_config" "$models_onnx_config" "$models_mindspore_config" \
                             "$models_mindspore_train_config" "$models_tflite_posttraining_config" "$models_caffe_posttraining_config" \
                             "$models_tflite_awaretraining_config" "$models_weightquant_config" "$models_weightquant_7bit_config" \
                             "$models_weightquant_9bit_config" "$models_for_process_only_config")
    # Convert models:
    # $1:cfgFileList; $2:inModelPath; $3:outModelPath; $4:logFile; $5:resultFile;
    Convert "${x86_cfg_file_list[*]}" $models_path $ms_models_path $run_converter_log_file $run_converter_result_file
}

# Run on x86 platform:
function Run_x86() {
    echo 'cd  '${x86_path}'/mindspore-lite-'${version}'-linux-x64' >> "${run_x86_log_file}"
    cd ${x86_path}/mindspore-lite-${version}-linux-x64 || return 1
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./runtime/lib
    cp tools/benchmark/benchmark ./ || exit 1

    # Run tflite parallel split converted models:
    while read line; do
        model_name=${line%;*}
        length=${#model_name}
        input_shapes=${line:length+1}
        if [[ $model_name == \#* ]]; then
          continue
        fi
        echo ${model_name} >> "${run_x86_log_file}"
        echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_1_1_parallel_split.ms --inDataFile='${models_path}'/input_output/input/'${model_name}'.ms.bin --inputShapes='${input_shapes}' --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'.ms.out' >> "${run_x86_log_file}"
        ./benchmark --modelFile=${ms_models_path}/${model_name}_1_1_parallel_split.ms --inDataFile=${models_path}/input_output/input/${model_name}.ms.bin --inputShapes=${input_shapes} --benchmarkDataFile=${models_path}/input_output/output/${model_name}.ms.out >> "${run_x86_log_file}"
        if [ $? = 0 ]; then
            run_result='x86_Parallel_Split: '${model_name}_1_1_parallel_split' pass'; echo ${run_result} >> ${run_benchmark_result_file}
        else
            run_result='x86_Parallel_Split: '${model_name}_1_1_parallel_split' failed'; echo ${run_result} >> ${run_benchmark_result_file}; return 1
        fi

        echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_1_2_parallel_split.ms --inDataFile='${models_path}'/input_output/input/'${model_name}'.ms.bin --inputShapes='${input_shapes}' --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'.ms.out' >> "${run_x86_log_file}"
        ./benchmark --modelFile=${ms_models_path}/${model_name}_1_2_parallel_split.ms --inDataFile=${models_path}/input_output/input/${model_name}.ms.bin --inputShapes=${input_shapes} --benchmarkDataFile=${models_path}/input_output/output/${model_name}.ms.out >> "${run_x86_log_file}"
        if [ $? = 0 ]; then
            run_result='x86_Parallel_Split: '${model_name}_1_2_parallel_split' pass'; echo ${run_result} >> ${run_benchmark_result_file}
        else
            run_result='x86_Parallel_Split: '${model_name}_1_2_parallel_split' failed'; echo ${run_result} >> ${run_benchmark_result_file}; return 1
        fi

        echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_1_3_parallel_split.ms --inDataFile='${models_path}'/input_output/input/'${model_name}'.ms.bin --inputShapes='${input_shapes}' --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'.ms.out' >> "${run_x86_log_file}"
        ./benchmark --modelFile=${ms_models_path}/${model_name}_1_3_parallel_split.ms --inDataFile=${models_path}/input_output/input/${model_name}.ms.bin --inputShapes=${input_shapes} --benchmarkDataFile=${models_path}/input_output/output/${model_name}.ms.out >> "${run_x86_log_file}"
        if [ $? = 0 ]; then
            run_result='x86_Parallel_Split: '${model_name}_1_3_parallel_split' pass'; echo ${run_result} >> ${run_benchmark_result_file}
        else
            run_result='x86_Parallel_Split: '${model_name}_1_3_parallel_split' failed'; echo ${run_result} >> ${run_benchmark_result_file}; return 1
        fi
    done < ${models_tflite_parallel_split_config}

    # Run tflite post training quantization converted models:
    while read line; do
        posttraining_line_info=${line}
        if [[ $posttraining_line_info == \#* ]]; then
          continue
        fi
        model_name=`echo ${posttraining_line_info}|awk -F ' ' '{print $1}'`
        accuracy_limit=`echo ${posttraining_line_info}|awk -F ' ' '{print $2}'`
        transformer_data_path="${models_path}/input_output/input"
        echo ${model_name} >> "${run_x86_log_file}"
        if [[ $model_name == "mobilenet.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=/home/workspace/mindspore_dataset/mslite/quantTraining/mnist_calibration_data/00099.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=/home/workspace/mindspore_dataset/mslite/quantTraining/mnist_calibration_data/00099.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_log_file}"
        fi
        if [[ $model_name == "transformer_20200831_encoder_fp32.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_0-35.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_35.bin,${transformer_data_path}/encoder_buffer_in_0.bin,${transformer_data_path}/encoder_buffer_in_1.bin,${transformer_data_path}/encoder_buffer_in_4.bin,${transformer_data_path}/encoder_buffer_in_2.bin,${transformer_data_path}/encoder_buffer_in_3.bin,${transformer_data_path}/encoder_buffer_in_7.bin,${transformer_data_path}/encoder_buffer_in_5.bin,${transformer_data_path}/encoder_buffer_in_6.bin,${transformer_data_path}/encoder_buffer_in_10.bin,${transformer_data_path}/encoder_buffer_in_8.bin,${transformer_data_path}/encoder_buffer_in_9.bin,${transformer_data_path}/encoder_buffer_in_11.bin,${transformer_data_path}/encoder_buffer_in_12.bin,${transformer_data_path}/encoder_buffer_in_15.bin,${transformer_data_path}/encoder_buffer_in_13.bin,${transformer_data_path}/encoder_buffer_in_14.bin,${transformer_data_path}/encoder_buffer_in_18.bin,${transformer_data_path}/encoder_buffer_in_16.bin,${transformer_data_path}/encoder_buffer_in_17.bin,${transformer_data_path}/encoder_buffer_in_21.bin,${transformer_data_path}/encoder_buffer_in_19.bin,${transformer_data_path}/encoder_buffer_in_20.bin,${transformer_data_path}/encoder_buffer_in_22.bin,${transformer_data_path}/encoder_buffer_in_23.bin,${transformer_data_path}/encoder_buffer_in_26.bin,${transformer_data_path}/encoder_buffer_in_24.bin,${transformer_data_path}/encoder_buffer_in_25.bin,${transformer_data_path}/encoder_buffer_in_29.bin,${transformer_data_path}/encoder_buffer_in_27.bin,${transformer_data_path}/encoder_buffer_in_28.bin,${transformer_data_path}/encoder_buffer_in_32.bin,${transformer_data_path}/encoder_buffer_in_30.bin,${transformer_data_path}/encoder_buffer_in_31.bin,${transformer_data_path}/encoder_buffer_in_33.bin,${transformer_data_path}/encoder_buffer_in_34.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_log_file}"
        fi
        if [[ $model_name == "transformer_20200831_decoder_fp32.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=${transformer_data_path}/decoder_buffer_in_0-10.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=${transformer_data_path}/decoder_buffer_in_9.bin,${transformer_data_path}/decoder_buffer_in_2.bin,${transformer_data_path}/decoder_buffer_in_0.bin,${transformer_data_path}/decoder_buffer_in_1.bin,${transformer_data_path}/decoder_buffer_in_5.bin,${transformer_data_path}/decoder_buffer_in_3.bin,${transformer_data_path}/decoder_buffer_in_4.bin,${transformer_data_path}/decoder_buffer_in_8.bin,${transformer_data_path}/decoder_buffer_in_6.bin,${transformer_data_path}/decoder_buffer_in_7.bin,${transformer_data_path}/decoder_buffer_in_10.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_log_file}"
        fi
        if [ $? = 0 ]; then
            run_result='x86_posttraining: '${model_name}' pass'; echo ${run_result} >> ${run_benchmark_result_file}
        else
            run_result='x86_posttraining: '${model_name}' failed'; echo ${run_result} >> ${run_benchmark_result_file}; return 1
        fi
    done < ${models_tflite_posttraining_config}

    # Prepare the config file list
    local x86_cfg_file_list=("$models_tf_config" "$models_tflite_config" "$models_caffe_config" "$models_onnx_config" "$models_mindspore_config" \
                             "$models_mindspore_train_config" "$models_caffe_posttraining_config" "$models_tflite_awaretraining_config" \
                             "$models_weightquant_config" "$models_for_process_only_config")
    # Run converted models:
    # $1:cfgFileList; $2:modelPath; $3:dataPath; $4:logFile; $5:resultFile; $6:platform; $7:processor; $8:phoneId;
    Run_Benchmark "${x86_cfg_file_list[*]}" $ms_models_path $models_path $run_x86_log_file $run_benchmark_result_file 'x86' 'CPU' ''
}

# Run on x86 sse platform:
function Run_x86_sse() {
    cd ${x86_path}/sse || exit 1
    tar -zxf mindspore-lite-${version}-linux-x64.tar.gz || exit 1
    cd ${x86_path}/sse/mindspore-lite-${version}-linux-x64 || return 1
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./runtime/lib
    cp tools/benchmark/benchmark ./ || exit 1

    # Run tflite post training quantization converted models:
    while read line; do
        posttraining_line_info=${line}
        if [[ $posttraining_line_info == \#* ]]; then
          continue
        fi
        model_name=`echo ${posttraining_line_info}|awk -F ' ' '{print $1}'`
        accuracy_limit=`echo ${posttraining_line_info}|awk -F ' ' '{print $2}'`
        transformer_data_path="${models_path}/input_output/input"
        echo ${model_name} >> "${run_x86_sse_log_file}"
        if [[ $model_name == "mobilenet.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=/home/workspace/mindspore_dataset/mslite/quantTraining/mnist_calibration_data/00099.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_sse_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=/home/workspace/mindspore_dataset/mslite/quantTraining/mnist_calibration_data/00099.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_sse_log_file}"
        fi
        if [[ $model_name == "transformer_20200831_encoder_fp32.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_0-35.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_sse_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_35.bin,${transformer_data_path}/encoder_buffer_in_0.bin,${transformer_data_path}/encoder_buffer_in_1.bin,${transformer_data_path}/encoder_buffer_in_4.bin,${transformer_data_path}/encoder_buffer_in_2.bin,${transformer_data_path}/encoder_buffer_in_3.bin,${transformer_data_path}/encoder_buffer_in_7.bin,${transformer_data_path}/encoder_buffer_in_5.bin,${transformer_data_path}/encoder_buffer_in_6.bin,${transformer_data_path}/encoder_buffer_in_10.bin,${transformer_data_path}/encoder_buffer_in_8.bin,${transformer_data_path}/encoder_buffer_in_9.bin,${transformer_data_path}/encoder_buffer_in_11.bin,${transformer_data_path}/encoder_buffer_in_12.bin,${transformer_data_path}/encoder_buffer_in_15.bin,${transformer_data_path}/encoder_buffer_in_13.bin,${transformer_data_path}/encoder_buffer_in_14.bin,${transformer_data_path}/encoder_buffer_in_18.bin,${transformer_data_path}/encoder_buffer_in_16.bin,${transformer_data_path}/encoder_buffer_in_17.bin,${transformer_data_path}/encoder_buffer_in_21.bin,${transformer_data_path}/encoder_buffer_in_19.bin,${transformer_data_path}/encoder_buffer_in_20.bin,${transformer_data_path}/encoder_buffer_in_22.bin,${transformer_data_path}/encoder_buffer_in_23.bin,${transformer_data_path}/encoder_buffer_in_26.bin,${transformer_data_path}/encoder_buffer_in_24.bin,${transformer_data_path}/encoder_buffer_in_25.bin,${transformer_data_path}/encoder_buffer_in_29.bin,${transformer_data_path}/encoder_buffer_in_27.bin,${transformer_data_path}/encoder_buffer_in_28.bin,${transformer_data_path}/encoder_buffer_in_32.bin,${transformer_data_path}/encoder_buffer_in_30.bin,${transformer_data_path}/encoder_buffer_in_31.bin,${transformer_data_path}/encoder_buffer_in_33.bin,${transformer_data_path}/encoder_buffer_in_34.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_sse_log_file}"
        fi
        if [[ $model_name == "transformer_20200831_decoder_fp32.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_0-10.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_sse_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=${transformer_data_path}/decoder_buffer_in_9.bin,${transformer_data_path}/decoder_buffer_in_2.bin,${transformer_data_path}/decoder_buffer_in_0.bin,${transformer_data_path}/decoder_buffer_in_1.bin,${transformer_data_path}/decoder_buffer_in_5.bin,${transformer_data_path}/decoder_buffer_in_3.bin,${transformer_data_path}/decoder_buffer_in_4.bin,${transformer_data_path}/decoder_buffer_in_8.bin,${transformer_data_path}/decoder_buffer_in_6.bin,${transformer_data_path}/decoder_buffer_in_7.bin,${transformer_data_path}/decoder_buffer_in_10.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_sse_log_file}"
        fi
        if [ $? = 0 ]; then
            run_result='x86_sse: '${model_name}' pass'; echo ${run_result} >> ${run_benchmark_result_file}
        else
            run_result='x86_sse: '${model_name}' failed'; echo ${run_result} >> ${run_benchmark_result_file}; return 1
        fi
    done < ${models_tflite_posttraining_config}

    # Prepare the config file list
    local sse_cfg_file_list=("$models_tf_config" "$models_tflite_config" "$models_caffe_config" "$models_onnx_config" "$models_mindspore_config" \
                             "$models_mindspore_train_config" "$models_caffe_posttraining_config" "$models_tflite_awaretraining_config" \
                             "$models_weightquant_config" "$models_for_process_only_config")
    # Run converted models:
    # $1:cfgFileList; $2:modelPath; $3:dataPath; $4:logFile; $5:resultFile; $6:platform; $7:processor; $8:phoneId;
    Run_Benchmark "${sse_cfg_file_list[*]}" $ms_models_path $models_path $run_x86_sse_log_file $run_benchmark_result_file 'x86' 'CPU' ''
}

# Run on x86 avx platform:
function Run_x86_avx() {
    cd ${x86_path}/avx || exit 1
    tar -zxf mindspore-lite-${version}-linux-x64.tar.gz || exit 1
    cd ${x86_path}/avx/mindspore-lite-${version}-linux-x64 || return 1
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./runtime/lib
    cp tools/benchmark/benchmark ./ || exit 1

    # Run tflite post training quantization converted models:
    while read line; do
        posttraining_line_info=${line}
        if [[ $posttraining_line_info == \#* ]]; then
          continue
        fi
        model_name=`echo ${posttraining_line_info}|awk -F ' ' '{print $1}'`
        accuracy_limit=`echo ${posttraining_line_info}|awk -F ' ' '{print $2}'`
        transformer_data_path="${models_path}/input_output/input"
        echo ${model_name} >> "${run_x86_avx_log_file}"
        if [[ $model_name == "mobilenet.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=/home/workspace/mindspore_dataset/mslite/quantTraining/mnist_calibration_data/00099.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_avx_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=/home/workspace/mindspore_dataset/mslite/quantTraining/mnist_calibration_data/00099.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_avx_log_file}"
        fi
        if [[ $model_name == "transformer_20200831_encoder_fp32.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_0-35.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_avx_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_35.bin,${transformer_data_path}/encoder_buffer_in_0.bin,${transformer_data_path}/encoder_buffer_in_1.bin,${transformer_data_path}/encoder_buffer_in_4.bin,${transformer_data_path}/encoder_buffer_in_2.bin,${transformer_data_path}/encoder_buffer_in_3.bin,${transformer_data_path}/encoder_buffer_in_7.bin,${transformer_data_path}/encoder_buffer_in_5.bin,${transformer_data_path}/encoder_buffer_in_6.bin,${transformer_data_path}/encoder_buffer_in_10.bin,${transformer_data_path}/encoder_buffer_in_8.bin,${transformer_data_path}/encoder_buffer_in_9.bin,${transformer_data_path}/encoder_buffer_in_11.bin,${transformer_data_path}/encoder_buffer_in_12.bin,${transformer_data_path}/encoder_buffer_in_15.bin,${transformer_data_path}/encoder_buffer_in_13.bin,${transformer_data_path}/encoder_buffer_in_14.bin,${transformer_data_path}/encoder_buffer_in_18.bin,${transformer_data_path}/encoder_buffer_in_16.bin,${transformer_data_path}/encoder_buffer_in_17.bin,${transformer_data_path}/encoder_buffer_in_21.bin,${transformer_data_path}/encoder_buffer_in_19.bin,${transformer_data_path}/encoder_buffer_in_20.bin,${transformer_data_path}/encoder_buffer_in_22.bin,${transformer_data_path}/encoder_buffer_in_23.bin,${transformer_data_path}/encoder_buffer_in_26.bin,${transformer_data_path}/encoder_buffer_in_24.bin,${transformer_data_path}/encoder_buffer_in_25.bin,${transformer_data_path}/encoder_buffer_in_29.bin,${transformer_data_path}/encoder_buffer_in_27.bin,${transformer_data_path}/encoder_buffer_in_28.bin,${transformer_data_path}/encoder_buffer_in_32.bin,${transformer_data_path}/encoder_buffer_in_30.bin,${transformer_data_path}/encoder_buffer_in_31.bin,${transformer_data_path}/encoder_buffer_in_33.bin,${transformer_data_path}/encoder_buffer_in_34.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_avx_log_file}"
        fi
        if [[ $model_name == "transformer_20200831_decoder_fp32.tflite" ]]; then
            echo './benchmark --modelFile='${ms_models_path}'/'${model_name}'_posttraining.ms --inDataFile=${transformer_data_path}/encoder_buffer_in_0-10.bin --benchmarkDataFile='${models_path}'/input_output/output/'${model_name}'_posttraining.ms.out' --accuracyThreshold=${accuracy_limit} >> "${run_x86_avx_log_file}"
            ./benchmark --modelFile=${ms_models_path}/${model_name}_posttraining.ms --inDataFile=${transformer_data_path}/decoder_buffer_in_9.bin,${transformer_data_path}/decoder_buffer_in_2.bin,${transformer_data_path}/decoder_buffer_in_0.bin,${transformer_data_path}/decoder_buffer_in_1.bin,${transformer_data_path}/decoder_buffer_in_5.bin,${transformer_data_path}/decoder_buffer_in_3.bin,${transformer_data_path}/decoder_buffer_in_4.bin,${transformer_data_path}/decoder_buffer_in_8.bin,${transformer_data_path}/decoder_buffer_in_6.bin,${transformer_data_path}/decoder_buffer_in_7.bin,${transformer_data_path}/decoder_buffer_in_10.bin --benchmarkDataFile=${models_path}/input_output/output/${model_name}_posttraining.ms.out --accuracyThreshold=${accuracy_limit} >> "${run_x86_avx_log_file}"
        fi
        if [ $? = 0 ]; then
            run_result='x86_avx: '${model_name}' pass'; echo ${run_result} >> ${run_benchmark_result_file}
        else
            run_result='x86_avx: '${model_name}' failed'; echo ${run_result} >> ${run_benchmark_result_file}; return 1
        fi
    done < ${models_tflite_posttraining_config}

    # Prepare the config file list
    local avx_cfg_file_list=("$models_tf_config" "$models_tflite_config" "$models_caffe_config" "$models_onnx_config" "$models_mindspore_config" \
                             "$models_mindspore_train_config" "$models_caffe_posttraining_config" "$models_tflite_awaretraining_config" \
                             "$models_weightquant_config" "$models_for_process_only_config")
    # Run converted models:
    # $1:cfgFileList; $2:modelPath; $3:dataPath; $4:logFile; $5:resultFile; $6:platform; $7:processor; $8:phoneId; $9:benchmark_mode
    Run_Benchmark "${avx_cfg_file_list[*]}" $ms_models_path $models_path $run_x86_avx_log_file $run_benchmark_result_file 'x86' 'CPU' ''
}

# Run on x86 java platform:
function Run_x86_java() {
    cd ${x86_path} || exit 1
    mkdir java || exit 1
    cp ${x86_path}/avx/mindspore-lite-${version}-linux-x64.tar.gz ./java/ || exit 1
    cd ./java || exit 1
    tar -zxf mindspore-lite-${version}-linux-x64.tar.gz || exit 1
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${x86_path}/java/mindspore-lite-${version}-linux-x64/runtime/lib:${x86_path}/java/mindspore-lite-${version}-linux-x64/runtime/third_party/libjpeg-turbo/lib
    # compile benchmark
    echo "javac -cp ${x86_path}/java/mindspore-lite-${version}-linux-x64/runtime/lib/mindspore-lite-java.jar ${basepath}/java/src/main/java/Benchmark.java -d ."
    javac -cp ${x86_path}/java/mindspore-lite-${version}-linux-x64/runtime/lib/mindspore-lite-java.jar ${basepath}/java/src/main/java/Benchmark.java -d .

    count=0
    # Run tflite converted models:
    while read line; do
        # only run top5.
        count=`expr ${count}+1`
        if [[ ${count} -gt 5 ]]; then
            break
        fi
        model_name=${line}
        if [[ $model_name == \#* ]]; then
          continue
        fi
        echo ${model_name} >> "${run_x86_java_log_file}"
        echo "java -classpath .:${x86_path}/java/mindspore-lite-${version}-linux-x64/runtime/lib/mindspore-lite-java.jar Benchmark ${ms_models_path}/${model_name}.ms '${models_path}'/input_output/input/${model_name}.ms.bin '${models_path}'/input_output/output/${model_name}.ms.out 1" >> "${run_x86_java_log_file}"
        java -classpath .:${x86_path}/java/mindspore-lite-${version}-linux-x64/runtime/lib/mindspore-lite-java.jar Benchmark ${ms_models_path}/${model_name}.ms ${models_path}/input_output/input/${model_name}.ms.bin ${models_path}/input_output/output/${model_name}.ms.out 1
        if [ $? = 0 ]; then
            run_result='x86_java: '${model_name}' pass'; echo ${run_result} >> ${run_benchmark_result_file}
        else
            run_result='x86_java: '${model_name}' failed'; echo ${run_result} >> ${run_benchmark_result_file}; return 1
        fi
    done < ${models_tflite_config}
}

basepath=$(pwd)
echo ${basepath}
#set -e

# Example:sh run_benchmark_x86.sh -r /home/temp_test -m /home/temp_test/models -e arm_cpu
while getopts "r:m:e:" opt; do
    case ${opt} in
        r)
            release_path=${OPTARG}
            echo "release_path is ${OPTARG}"
            ;;
        m)
            models_path=${OPTARG}
            echo "models_path is ${OPTARG}"
            ;;
        e)
            backend=${OPTARG}
            echo "backend is ${OPTARG}"
            ;;
        ?)
        echo "unknown para"
        exit 1;;
    esac
done

# mkdir train

x86_path=${release_path}/ubuntu_x86
file_name=$(ls ${x86_path}/*-linux-x64.tar.gz)
IFS="-" read -r -a file_name_array <<< "$file_name"
version=${file_name_array[2]}

# Set models config filepath
models_tflite_parallel_split_config=${basepath}/../config/models_parallel_split.cfg
models_tflite_config=${basepath}/../config/models_tflite.cfg
models_tf_config=${basepath}/../config/models_tf.cfg
models_caffe_config=${basepath}/../config/models_caffe.cfg
models_tflite_awaretraining_config=${basepath}/../config/models_tflite_awaretraining.cfg
models_tflite_posttraining_config=${basepath}/../config/models_tflite_posttraining.cfg
models_caffe_posttraining_config=${basepath}/../config/models_caffe_posttraining.cfg
models_onnx_config=${basepath}/../config/models_onnx.cfg
models_mindspore_config=${basepath}/../config/models_mindspore.cfg
models_mindspore_train_config=${basepath}/../config/models_mindspore_train.cfg
models_weightquant_7bit_config=${basepath}/../config/models_weightquant_7bit.cfg
models_weightquant_9bit_config=${basepath}/../config/models_weightquant_9bit.cfg
models_weightquant_config=${basepath}/../config/models_weightquant.cfg
models_for_process_only_config=${basepath}/../config/models_for_process_only.cfg

ms_models_path=${basepath}/ms_models

# Write converter result to temp file
run_converter_log_file=${basepath}/run_converter_log.txt
echo ' ' > ${run_converter_log_file}

run_converter_result_file=${basepath}/run_converter_result.txt
echo ' ' > ${run_converter_result_file}

# Run converter
echo "start Run converter ..."
Run_Converter
Run_converter_status=$?
sleep 1

# Check converter result and return value
if [[ ${Run_converter_status} = 0 ]];then
    echo "Run converter success"
    Print_Converter_Result $run_converter_result_file
else
    echo "Run converter failed"
    cat ${run_converter_log_file}
    Print_Converter_Result $run_converter_result_file
    exit 1
fi

# Write benchmark result to temp file
run_benchmark_result_file=${basepath}/run_benchmark_result.txt
echo ' ' > ${run_benchmark_result_file}

run_x86_log_file=${basepath}/run_x86_log.txt
echo 'run x86 logs: ' > ${run_x86_log_file}
run_x86_sse_log_file=${basepath}/run_x86_sse_log.txt
echo 'run x86 sse logs: ' > ${run_x86_sse_log_file}
run_x86_avx_log_file=${basepath}/run_x86_avx_log.txt
echo 'run x86 avx logs: ' > ${run_x86_avx_log_file}
run_x86_java_log_file=${basepath}/run_x86_java_log.txt
echo 'run x86 java logs: ' > ${run_x86_java_log_file}

backend=${backend:-"all"}
isFailed=0

if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86-avx" ]]; then
    # Run on x86-avx
    echo "start Run x86 avx ..."
    Run_x86_avx &
    Run_x86_avx_PID=$!
    sleep 1
fi
if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86" ]]; then
    # Run on x86
    echo "start Run x86 ..."
    Run_x86 &
    Run_x86_PID=$!
    sleep 1
fi
if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86-sse" ]]; then
    # Run on x86-sse
    echo "start Run x86 sse ..."
    Run_x86_sse &
    Run_x86_sse_PID=$!
    sleep 1
fi
if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86-java" ]]; then
    # Run on x86-java
    echo "start Run x86 java ..."
    Run_x86_java &
    Run_x86_java_PID=$!
    sleep 1
fi

if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86-avx" ]]; then
    wait ${Run_x86_avx_PID}
    Run_x86_avx_status=$?

    if [[ ${Run_x86_avx_status} != 0 ]];then
        echo "Run_x86 avx failed"
        cat ${run_x86_avx_log_file}
        isFailed=1
    fi
fi
if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86-sse" ]]; then
    wait ${Run_x86_sse_PID}
    Run_x86_sse_status=$?

    if [[ ${Run_x86_sse_status} != 0 ]];then
        echo "Run_x86 sse failed"
        cat ${run_x86_sse_log_file}
        isFailed=1
    fi
fi
if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86" ]]; then
    wait ${Run_x86_PID}
    Run_x86_status=$?

    # Check benchmark result and return value
    if [[ ${Run_x86_status} != 0 ]];then
        echo "Run_x86 failed"
        cat ${run_x86_log_file}
        isFailed=1
    fi
fi
if [[ $backend == "all" || $backend == "x86-all" || $backend == "x86-java" ]]; then
    wait ${Run_x86_java_PID}
    Run_x86_java_status=$?

    if [[ ${Run_x86_java_status} != 0 ]];then
        echo "Run_x86 java failed"
        cat ${run_x86_java_log_file}
        isFailed=1
    fi
fi

echo "Run_x86 and Run_x86_sse and Run_x86_avx and is ended"
Print_Benchmark_Result $run_benchmark_result_file
exit ${isFailed}
