#!/bin/bash

basepath=$(pwd)
echo ${basepath}
test_dir=${basepath}/../../
echo ${test_dir}

# Example:sh run_ut_gpu.sh -r /home/temp_test -d "8KE5T19620002408"
while getopts "r:d:" opt; do
    case ${opt} in
        d)
            device_id=${OPTARG}
            echo "device_id is ${OPTARG}"
            ;;
        ?)
        echo "unknown para"
        exit 1;;
    esac
done

ut_test_path=${basepath}/ut_test
rm -rf ${ut_test_path}
mkdir -p ${ut_test_path}

run_ut_result_file=${basepath}/run_gpu_ut_result.txt
echo ' ' > ${run_ut_result_file}
run_gpu_ut_log_file=${basepath}/run_gpu_ut_log.txt
echo 'run gpu ut logs: ' > ${run_gpu_ut_log_file}

ut_gpu_config=${test_dir}/config/ut_gpu.cfg

function Run_gpu_ut() {
    cp -a ${test_dir}/build/lite-test ${ut_test_path}/lite-test || exit 1
    cp -r ${test_dir}/ut/src/runtime/kernel/opencl/test_data ${ut_test_path} || exit 1

    # adb push all needed files to the phone
    adb -s ${device_id} push ${ut_test_path} /data/local/tmp/ > adb_push_log.txt

    # run adb ,run session ,check the result:
    echo 'rm -rf /data/local/tmp/ut_test' > adb_cmd.txt
    echo 'cd  /data/local/tmp/ut_test' > adb_cmd.txt
    echo 'cp  /data/local/tmp/libc++_shared.so ./' >> adb_cmd.txt
    echo 'cp  /data/local/tmp/libgtest.so ./' >> adb_cmd.txt
    echo 'cp  /data/local/tmp/libhiai.so ./' >> adb_cmd.txt
    echo 'cp  /data/local/tmp/libhiai_ir.so ./' >> adb_cmd.txt
    echo 'cp  /data/local/tmp/libhiai_ir_build.so ./' >> adb_cmd.txt
    echo 'chmod 777 lite-test' >> adb_cmd.txt

    adb -s ${device_id} shell < adb_cmd.txt

    # Run npu converted models:
    while read line; do
        echo 'cd  /data/local/tmp/ut_test' > adb_run_cmd.txt
        echo 'export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/data/local/tmp/ut_test;./lite-test --gtest_filter='${line} >> "${run_gpu_ut_log_file}"
        echo 'export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/data/local/tmp/ut_test;./lite-test --gtest_filter='${line} >> adb_run_cmd.txt
        adb -s ${device_id} shell < adb_run_cmd.txt >> "${run_gpu_ut_log_file}"
        if [ $? = 0 ]; then
            run_result='arm64_gpu_ut: '${line}' pass'; echo ${run_result} >> ${run_ut_result_file}
        else
            run_result='arm64_gpu_ut: '${line}' failed'; echo ${run_result} >> ${run_ut_result_file}; return 1
        fi
    done < ${ut_gpu_config}
}

Run_gpu_ut
Run_gpu_ut_status=$?

cat ${run_ut_result_file}
if [[ $Run_gpu_ut_status == 1 ]]; then
    cat ${run_gpu_ut_log_file}
    exit 1
fi
exit 0