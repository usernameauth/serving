# Description: Tensorflow Serving core test utils.

load("//tensorflow_serving:serving.bzl", "serving_proto_library")

package(
    default_visibility = [
        "//tensorflow_serving:internal",
    ],
    features = ["-layering_check"],
)

licenses(["notice"])

filegroup(
    name = "all_files",
    srcs = glob(
        ["**/*"],
        exclude = [
            "**/METADATA",
            "**/OWNERS",
        ],
    ),
)

# Main program for tests
cc_library(
    name = "test_main",
    testonly = 1,
    srcs = ["test_main.cc"],
    linkopts = ["-lm"],
    tags = ["keep_dep"],  # Tell build_cleaner to keep dependencies on this.
    deps = [
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest",
        "@org_tensorflow//tensorflow/core:lib",
        "@org_tensorflow//tensorflow/core:test",
        "@org_tensorflow//tensorflow/core:testlib",
        "@org_tensorflow//tensorflow/core/platform",
        "@org_tensorflow//tensorflow/core/platform:test_benchmark",
        "@org_tensorflow//tensorflow/core/platform:types",
    ],
)

cc_library(
    name = "mock_loader",
    testonly = 1,
    hdrs = ["mock_loader.h"],
    deps = [
        "//tensorflow_serving/core:loader",
        "//tensorflow_serving/util:any_ptr",
        "@com_google_googletest//:gtest",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "fake_loader",
    testonly = 1,
    srcs = ["fake_loader.cc"],
    hdrs = ["fake_loader.h"],
    deps = [
        "//tensorflow_serving/core:loader",
        "//tensorflow_serving/util:any_ptr",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "fake_loader_source_adapter",
    testonly = 1,
    srcs = ["fake_loader_source_adapter.cc"],
    hdrs = ["fake_loader_source_adapter.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":fake_loader_source_adapter_cc_proto",
        "//tensorflow_serving/core:loader",
        "//tensorflow_serving/core:simple_loader",
        "//tensorflow_serving/core:source_adapter",
        "//tensorflow_serving/core:storage_path",
        "@org_tensorflow//tensorflow/core:lib",
    ],
    alwayslink = 1,
)

serving_proto_library(
    name = "fake_loader_source_adapter_proto",
    testonly = 1,
    srcs = ["fake_loader_source_adapter.proto"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "fake_storage_path_source_adapter",
    testonly = 1,
    srcs = ["fake_storage_path_source_adapter.cc"],
    hdrs = ["fake_storage_path_source_adapter.h"],
    deps = [
        "//tensorflow_serving/core:source_adapter",
        "//tensorflow_serving/core:storage_path",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "mock_storage_path_target",
    testonly = 1,
    hdrs = ["mock_storage_path_target.h"],
    deps = [
        "//tensorflow_serving/core:storage_path",
        "//tensorflow_serving/core:target",
        "@com_google_googletest//:gtest",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "availability_test_util",
    testonly = 1,
    srcs = ["availability_test_util.cc"],
    hdrs = ["availability_test_util.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tensorflow_serving/core:servable_state_monitor",
        "@com_google_absl//absl/types:optional",
        "@com_google_absl//absl/types:span",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "manager_test_util",
    testonly = 1,
    srcs = ["manager_test_util.cc"],
    hdrs = ["manager_test_util.h"],
    deps = [
        "//tensorflow_serving/core:aspired_versions_manager",
        "//tensorflow_serving/core:caching_manager",
    ],
)

cc_library(
    name = "servable_handle_test_util",
    testonly = 1,
    hdrs = ["servable_handle_test_util.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tensorflow_serving/core:manager",
        "//tensorflow_serving/core:servable_handle",
        "//tensorflow_serving/core:servable_id",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "mock_log_collector",
    testonly = 1,
    hdrs = ["mock_log_collector.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tensorflow_serving/core:log_collector",
        "@com_google_googletest//:gtest",
        "@com_google_protobuf//:protobuf",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "fake_log_collector",
    testonly = 1,
    hdrs = ["fake_log_collector.h"],
    deps = [
        "//tensorflow_serving/core:log_collector",
        "@com_google_protobuf//:protobuf",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "mock_request_logger",
    testonly = 1,
    hdrs = ["mock_request_logger.h"],
    deps = [
        "//tensorflow_serving/apis:logging_cc_proto",
        "//tensorflow_serving/config:logging_config_cc_proto",
        "//tensorflow_serving/core:log_collector",
        "//tensorflow_serving/core:request_logger",
        "@com_google_googletest//:gtest",
        "@com_google_protobuf//:protobuf",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "mock_server_request_logger",
    testonly = 1,
    hdrs = ["mock_server_request_logger.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tensorflow_serving/core:server_request_logger",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "mock_session",
    testonly = 1,
    hdrs = ["mock_session.h"],
    deps = [
        "@com_google_googletest//:gtest",
        "@org_tensorflow//tensorflow/core:core_cpu",
        "@org_tensorflow//tensorflow/core:lib",
    ],
)

cc_library(
    name = "session_test_util",
    testonly = 1,
    srcs = ["session_test_util.cc"],
    hdrs = ["session_test_util.h"],
    deps = [
        "@org_tensorflow//tensorflow/core:core_cpu",
        "@org_tensorflow//tensorflow/core:framework",
        "@org_tensorflow//tensorflow/core:lib",
    ],
    alwayslink = 1,
)

cc_library(
    name = "mock_prediction_stream_logger",
    testonly = 1,
    hdrs = ["mock_prediction_stream_logger.h"],
    deps = [
        "//tensorflow_serving/apis:logging_cc_proto",
        "//tensorflow_serving/apis:predict_cc_proto",
        "//tensorflow_serving/core:stream_logger",
        "@com_google_googletest//:gtest",
    ],
)
