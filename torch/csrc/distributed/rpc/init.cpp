#include <torch/csrc/python_headers.h>

#include <torch/csrc/distributed/rpc/FutureMessage.h>
#include <torch/csrc/distributed/rpc/ProcessGroupAgent.h>
#include <torch/csrc/distributed/rpc/RpcAgent.h>
#include <torch/csrc/distributed/rpc/functions.h>
#include <torch/csrc/distributed/rpc/python_functions.h>
#include <torch/csrc/jit/pybind_utils.h>
#include <torch/csrc/utils/object_ptr.h>
#include <torch/csrc/utils/pybind.h>
#include <torch/types.h>


namespace torch {
namespace distributed {
namespace rpc {

namespace {

template <typename T>
using shared_ptr_class_ = py::class_<T, std::shared_ptr<T>>;

PyObject* rpc_init(PyObject* /* unused */) {
  auto dist_module = THPObjectPtr(PyImport_ImportModule("torch.distributed"));
  if (!dist_module) {
    throw python_error();
  }

  auto module = py::handle(dist_module).cast<py::module>();

  auto rpcAgent = shared_ptr_class_<RpcAgent>(module, "RpcAgent")
      .def("join",
           &RpcAgent::join,
           py::call_guard<py::gil_scoped_release>())
      .def("sync",
           &RpcAgent::sync,
           py::call_guard<py::gil_scoped_release>());

  auto futureMessage = shared_ptr_class_<FutureMessage>(module, "FutureMessage")
      .def("wait",
          [&](FutureMessage& fut) {
            return to_py_obj(fut.wait());
          },
          py::call_guard<py::gil_scoped_release>());

  auto processGroupAgent =
      shared_ptr_class_<ProcessGroupAgent>(
          module, "ProcessGroupAgent", rpcAgent)
          .def(py::init<std::string,
                        std::shared_ptr<::c10d::ProcessGroup>>())
          .def("get_id",
               &ProcessGroupAgent::getId,
               py::call_guard<py::gil_scoped_release>())
          .def("get_worker_id",
               &ProcessGroupAgent::getWorkerId,
               py::call_guard<py::gil_scoped_release>())
          .def("join",
               &ProcessGroupAgent::join,
               py::call_guard<py::gil_scoped_release>())
          .def("sync",
               &ProcessGroupAgent::sync,
               py::call_guard<py::gil_scoped_release>());

  module.def("invoke_rpc", [](
      RpcAgent& agent,
      uint64_t dst,
      const std::string& opName,
      const py::args& args,
      const py::kwargs& kwargs) {
    return py_rpc(agent, dst, opName, args, kwargs);
  });

  Py_RETURN_TRUE;
}

} // namespace

static PyMethodDef methods[] = {  // NOLINT
    {"_rpc_init", (PyCFunction)rpc_init, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}};

PyMethodDef* python_functions() {
  return methods;
}

} // namespace rpc
} // namespace distributed
} // namespace torch
