#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <onnxruntime_cxx_api.h>

namespace py = pybind11;

PYBIND11_MODULE(onnxruntime_pybind11_gil_conversion, m) {
  py::class_<Ort::SessionOptions>(m, "SessionOptions")
      .def(py::init<>())
      .def("enable_profiling", &Ort::SessionOptions::EnableProfiling);
}