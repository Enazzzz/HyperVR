/**
 * Thin Python bindings for HyperVR — same Engine surface as C++.
 */
#include <Python.h>

#include <cstring>
#include <stdexcept>
#include <vector>

#include "hypervr/engine.hpp"

namespace {

/**
 * Python wrapper around hypervr::Engine.
 */
struct PyEngineObject {
	PyObject_HEAD
	hypervr::Engine* native = nullptr;
};

/**
 * Convert a Python sequence of 16 floats into a Mat4.
 */
bool parse_mat4(PyObject* obj, hypervr::Mat4& out) {
	if (!PySequence_Check(obj) || PySequence_Size(obj) != 16) {
		PyErr_SetString(PyExc_TypeError, "model matrix must be a sequence of 16 floats (column-major)");
		return false;
	}
	for (Py_ssize_t i = 0; i < 16; ++i) {
		PyObject* item = PySequence_GetItem(obj, i);
		if (!item) {
			return false;
		}
		out.m[static_cast<std::size_t>(i)] = static_cast<float>(PyFloat_AsDouble(item));
		Py_DECREF(item);
		if (PyErr_Occurred()) {
			return false;
		}
	}
	return true;
}

/**
 * Allocate and construct the native engine.
 */
static int PyEngine_init(PyEngineObject* self, PyObject* args, PyObject* kw) {
	const char* name = "HyperVR";
	int desktop_mode = 0;
	static char* kwlist[] = {const_cast<char*>("application_name"), const_cast<char*>("desktop_mode"), nullptr};
	if (!PyArg_ParseTupleAndKeywords(args, kw, "|sp", kwlist, &name, &desktop_mode)) {
		return -1;
	}
	try {
		self->native = new hypervr::Engine(name, desktop_mode != 0);
	} catch (const std::exception& ex) {
		PyErr_SetString(PyExc_RuntimeError, ex.what());
		return -1;
	}
	return 0;
}

/**
 * Destroy the native engine.
 */
static void PyEngine_dealloc(PyEngineObject* self) {
	delete self->native;
	self->native = nullptr;
	Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

static PyObject* PyEngine_is_running(PyEngineObject* self, PyObject*) {
	return PyBool_FromLong(self->native->is_running() ? 1 : 0);
}

static PyObject* PyEngine_poll_events(PyEngineObject* self, PyObject*) {
	self->native->poll_events();
	Py_RETURN_NONE;
}

static PyObject* PyEngine_begin_frame(PyEngineObject* self, PyObject*) {
	self->native->begin_frame();
	Py_RETURN_NONE;
}

static PyObject* PyEngine_end_frame(PyEngineObject* self, PyObject*) {
	self->native->end_frame();
	Py_RETURN_NONE;
}

static PyObject* PyEngine_set_desktop_mirror(PyEngineObject* self, PyObject* args) {
	int enabled = 1;
	if (!PyArg_ParseTuple(args, "p", &enabled)) {
		return nullptr;
	}
	self->native->set_desktop_mirror(enabled != 0);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_desktop_mirror_active(PyEngineObject* self, PyObject*) {
	if (self->native->desktop_mirror_active()) {
		Py_RETURN_TRUE;
	}
	Py_RETURN_FALSE;
}

static PyObject* PyEngine_set_desktop_camera(PyEngineObject* self, PyObject* args) {
	float x = 0.0f;
	float y = 1.4f;
	float z = -2.5f;
	float yaw = 3.14159265f;
	float pitch = 0.0f;
	if (!PyArg_ParseTuple(args, "fffff", &x, &y, &z, &yaw, &pitch)) {
		return nullptr;
	}
	self->native->set_desktop_camera(x, y, z, yaw, pitch);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_unlock_desktop_camera(PyEngineObject* self, PyObject*) {
	self->native->unlock_desktop_camera();
	Py_RETURN_NONE;
}

static PyObject* PyEngine_set_avatar_visible(PyEngineObject* self, PyObject* args) {
	int visible = 1;
	if (!PyArg_ParseTuple(args, "p", &visible)) {
		return nullptr;
	}
	self->native->set_avatar_visible(visible != 0);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_set_arms_visible(PyEngineObject* self, PyObject* args) {
	int visible = 1;
	if (!PyArg_ParseTuple(args, "p", &visible)) {
		return nullptr;
	}
	self->native->set_arms_visible(visible != 0);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_save_desktop_screenshot(PyEngineObject* self, PyObject* args) {
	const char* path = nullptr;
	if (!PyArg_ParseTuple(args, "s", &path)) {
		return nullptr;
	}
	if (self->native->save_desktop_screenshot(path)) {
		Py_RETURN_TRUE;
	}
	Py_RETURN_FALSE;
}

static PyObject* PyEngine_create_cube(PyEngineObject* self, PyObject*) {
	return PyLong_FromLong(self->native->create_cube());
}

static PyObject* PyEngine_create_room(PyEngineObject* self, PyObject* args) {
	float width = 12.0f;
	float depth = 12.0f;
	float height = 7.0f;
	if (!PyArg_ParseTuple(args, "|fff", &width, &depth, &height)) {
		return nullptr;
	}
	const hypervr::RoomVisual room = self->native->create_room(width, depth, height);
	// (mesh, width, depth, height)
	return Py_BuildValue("(ifff)", room.mesh, room.width, room.depth, room.height);
}

static PyObject* PyEngine_draw_room(PyEngineObject* self, PyObject* args) {
	hypervr::RoomVisual room{};
	if (!PyArg_ParseTuple(args, "(ifff)", &room.mesh, &room.width, &room.depth, &room.height)) {
		PyErr_Clear();
		if (!PyArg_ParseTuple(args, "i|fff", &room.mesh, &room.width, &room.depth, &room.height)) {
			return nullptr;
		}
	}
	self->native->draw_room(room);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_player_position(PyEngineObject* self, PyObject*) {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	self->native->player_position(x, y, z);
	return Py_BuildValue("(fff)", x, y, z);
}

static PyObject* PyEngine_player_velocity(PyEngineObject* self, PyObject*) {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	self->native->player_velocity(x, y, z);
	return Py_BuildValue("(fff)", x, y, z);
}

static PyObject* PyEngine_create_hand(PyEngineObject* self, PyObject* args) {
	int hand = 0;
	if (!PyArg_ParseTuple(args, "i", &hand)) {
		return nullptr;
	}
	const hypervr::HandVisual visual = self->native->create_hand(static_cast<hypervr::Hand>(hand));
	// (upper, lower, wrist, palm, finger_prox, finger_dist, thumb_prox, thumb_dist, joint_ball, side)
	return Py_BuildValue(
		"(iiiiiiiiii)",
		visual.upper,
		visual.lower,
		visual.wrist,
		visual.palm,
		visual.finger_prox,
		visual.finger_dist,
		visual.thumb_prox,
		visual.thumb_dist,
		visual.joint_ball,
		static_cast<int>(visual.side));
}

static PyObject* PyEngine_draw_hand(PyEngineObject* self, PyObject* args) {
	hypervr::HandVisual visual{};
	int side = 0;
	int close_fist = 0;
	if (!PyArg_ParseTuple(
			args,
			"(iiiiiiiiii)|p",
			&visual.upper,
			&visual.lower,
			&visual.wrist,
			&visual.palm,
			&visual.finger_prox,
			&visual.finger_dist,
			&visual.thumb_prox,
			&visual.thumb_dist,
			&visual.joint_ball,
			&side,
			&close_fist)) {
		PyErr_Clear();
		// Legacy 9-int tuple without joint_ball (balls skipped).
		if (!PyArg_ParseTuple(
				args,
				"(iiiiiiiii)|p",
				&visual.upper,
				&visual.lower,
				&visual.wrist,
				&visual.palm,
				&visual.finger_prox,
				&visual.finger_dist,
				&visual.thumb_prox,
				&visual.thumb_dist,
				&side,
				&close_fist)) {
			PyErr_Clear();
			if (!PyArg_ParseTuple(
					args,
					"iiiiiiiiii|p",
					&visual.upper,
					&visual.lower,
					&visual.wrist,
					&visual.palm,
					&visual.finger_prox,
					&visual.finger_dist,
					&visual.thumb_prox,
					&visual.thumb_dist,
					&visual.joint_ball,
					&side,
					&close_fist)) {
				return nullptr;
			}
		}
	}
	visual.side = static_cast<hypervr::Hand>(side);
	visual.upper_len = 0.36f;
	visual.lower_len = 0.32f;
	self->native->draw_hand(visual, close_fist != 0);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_draw_mesh(PyEngineObject* self, PyObject* args) {
	int mesh = -1;
	PyObject* mat_obj = nullptr;
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;
	if (!PyArg_ParseTuple(args, "iO|ffff", &mesh, &mat_obj, &r, &g, &b, &a)) {
		return nullptr;
	}
	hypervr::Mat4 model{};
	if (!parse_mat4(mat_obj, model)) {
		return nullptr;
	}
	self->native->draw_mesh(mesh, model, r, g, b, a);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_submit_draws(PyEngineObject* self, PyObject* args) {
	PyObject* seq = nullptr;
	if (!PyArg_ParseTuple(args, "O", &seq)) {
		return nullptr;
	}
	if (!PySequence_Check(seq)) {
		PyErr_SetString(PyExc_TypeError, "submit_draws expects a sequence of draw dicts/tuples");
		return nullptr;
	}
	const Py_ssize_t n = PySequence_Size(seq);
	std::vector<hypervr::DrawItem> draws;
	draws.reserve(static_cast<std::size_t>(n));
	for (Py_ssize_t i = 0; i < n; ++i) {
		PyObject* item = PySequence_GetItem(seq, i);
		if (!item) {
			return nullptr;
		}
		hypervr::DrawItem draw{};
		PyObject* mesh_obj = nullptr;
		PyObject* mat_obj = nullptr;
		if (PyDict_Check(item)) {
			mesh_obj = PyDict_GetItemString(item, "mesh");
			mat_obj = PyDict_GetItemString(item, "model");
			PyObject* color = PyDict_GetItemString(item, "color");
			if (!mesh_obj || !mat_obj) {
				Py_DECREF(item);
				PyErr_SetString(PyExc_TypeError, "draw dict needs mesh and model");
				return nullptr;
			}
			draw.mesh = static_cast<int>(PyLong_AsLong(mesh_obj));
			if (color && PySequence_Check(color) && PySequence_Size(color) >= 3) {
				draw.r = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(color, 0)));
				draw.g = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(color, 1)));
				draw.b = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(color, 2)));
				if (PySequence_Size(color) >= 4) {
					draw.a = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(color, 3)));
				}
			}
		} else if (PyTuple_Check(item) && PyTuple_Size(item) >= 2) {
			draw.mesh = static_cast<int>(PyLong_AsLong(PyTuple_GetItem(item, 0)));
			mat_obj = PyTuple_GetItem(item, 1);
			if (PyTuple_Size(item) >= 5) {
				draw.r = static_cast<float>(PyFloat_AsDouble(PyTuple_GetItem(item, 2)));
				draw.g = static_cast<float>(PyFloat_AsDouble(PyTuple_GetItem(item, 3)));
				draw.b = static_cast<float>(PyFloat_AsDouble(PyTuple_GetItem(item, 4)));
			}
			if (PyTuple_Size(item) >= 6) {
				draw.a = static_cast<float>(PyFloat_AsDouble(PyTuple_GetItem(item, 5)));
			}
		} else {
			Py_DECREF(item);
			PyErr_SetString(PyExc_TypeError, "each draw must be a dict or tuple");
			return nullptr;
		}
		if (!parse_mat4(mat_obj, draw.model)) {
			Py_DECREF(item);
			return nullptr;
		}
		draws.push_back(draw);
		Py_DECREF(item);
	}
	self->native->submit_draws(draws);
	Py_RETURN_NONE;
}

static PyObject* PyEngine_timings(PyEngineObject* self, PyObject*) {
	const hypervr::FrameTimings t = self->native->timings();
	return Py_BuildValue("(ffff)", t.wait_ms, t.cpu_record_ms, t.submit_ms, t.total_ms);
}

static PyObject* PyEngine_delta_time(PyEngineObject* self, PyObject*) {
	return PyFloat_FromDouble(self->native->delta_time());
}

static PyObject* PyEngine_backend_name(PyEngineObject* self, PyObject*) {
	return PyUnicode_FromString(self->native->backend_name().c_str());
}

static PyObject* PyEngine_controller_pose(PyEngineObject* self, PyObject* args) {
	int hand = 0;
	if (!PyArg_ParseTuple(args, "i", &hand)) {
		return nullptr;
	}
	hypervr::Mat4 pose{};
	if (!self->native->controller_pose(static_cast<hypervr::Hand>(hand), pose)) {
		Py_RETURN_NONE;
	}
	PyObject* list = PyList_New(16);
	for (int i = 0; i < 16; ++i) {
		PyList_SetItem(list, i, PyFloat_FromDouble(pose.m[static_cast<std::size_t>(i)]));
	}
	return list;
}

static PyObject* PyEngine_head_pose(PyEngineObject* self, PyObject*) {
	hypervr::Mat4 pose{};
	if (!self->native->head_pose(pose)) {
		Py_RETURN_NONE;
	}
	PyObject* list = PyList_New(16);
	for (int i = 0; i < 16; ++i) {
		PyList_SetItem(list, i, PyFloat_FromDouble(pose.m[static_cast<std::size_t>(i)]));
	}
	return list;
}

static PyObject* PyEngine_load_obj(PyEngineObject* self, PyObject* args) {
	const char* path = nullptr;
	float r = 0.22f;
	float g = 0.22f;
	float b = 0.24f;
	if (!PyArg_ParseTuple(args, "s|fff", &path, &r, &g, &b)) {
		return nullptr;
	}
	try {
		const int id = self->native->load_obj(path, r, g, b);
		return PyLong_FromLong(id);
	} catch (const std::exception& ex) {
		PyErr_SetString(PyExc_RuntimeError, ex.what());
		return nullptr;
	}
}

static PyObject* PyEngine_button_down(PyEngineObject* self, PyObject* args) {
	int hand = 0;
	int button = 0;
	if (!PyArg_ParseTuple(args, "ii", &hand, &button)) {
		return nullptr;
	}
	const bool down = self->native->button_down(
		static_cast<hypervr::Hand>(hand),
		static_cast<hypervr::ControllerButton>(button));
	return PyBool_FromLong(down ? 1 : 0);
}

static PyObject* PyEngine_load_mesh(PyEngineObject* self, PyObject* args) {
	PyObject* verts_obj = nullptr;
	PyObject* indices_obj = nullptr;
	if (!PyArg_ParseTuple(args, "OO", &verts_obj, &indices_obj)) {
		return nullptr;
	}
	if (!PySequence_Check(verts_obj) || !PySequence_Check(indices_obj)) {
		PyErr_SetString(PyExc_TypeError, "load_mesh expects vertex and index sequences");
		return nullptr;
	}
	const Py_ssize_t vcount = PySequence_Size(verts_obj);
	std::vector<hypervr::Vertex> vertices;
	vertices.reserve(static_cast<std::size_t>(vcount));
	for (Py_ssize_t i = 0; i < vcount; ++i) {
		PyObject* row = PySequence_GetItem(verts_obj, i);
		if (!row || !PySequence_Check(row) || PySequence_Size(row) < 6) {
			Py_XDECREF(row);
			PyErr_SetString(PyExc_TypeError, "each vertex needs 6 floats: px py pz r g b");
			return nullptr;
		}
		hypervr::Vertex v{};
		v.px = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(row, 0)));
		v.py = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(row, 1)));
		v.pz = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(row, 2)));
		v.r = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(row, 3)));
		v.g = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(row, 4)));
		v.b = static_cast<float>(PyFloat_AsDouble(PySequence_GetItem(row, 5)));
		vertices.push_back(v);
		Py_DECREF(row);
	}
	const Py_ssize_t icount = PySequence_Size(indices_obj);
	std::vector<std::uint32_t> indices;
	indices.reserve(static_cast<std::size_t>(icount));
	for (Py_ssize_t i = 0; i < icount; ++i) {
		PyObject* idx = PySequence_GetItem(indices_obj, i);
		indices.push_back(static_cast<std::uint32_t>(PyLong_AsUnsignedLong(idx)));
		Py_DECREF(idx);
	}
	const int id = self->native->load_mesh(vertices, indices);
	return PyLong_FromLong(id);
}

static PyMethodDef PyEngine_methods[] = {
	{"is_running", reinterpret_cast<PyCFunction>(PyEngine_is_running), METH_NOARGS, "True while XR session should run."},
	{"poll_events", reinterpret_cast<PyCFunction>(PyEngine_poll_events), METH_NOARGS, "Poll OpenXR events and actions."},
	{"begin_frame", reinterpret_cast<PyCFunction>(PyEngine_begin_frame), METH_NOARGS, "Wait and begin XR frame."},
	{"end_frame", reinterpret_cast<PyCFunction>(PyEngine_end_frame), METH_NOARGS, "Stereo submit via xrEndFrame."},
	{"set_desktop_mirror", reinterpret_cast<PyCFunction>(PyEngine_set_desktop_mirror), METH_VARARGS, "Enable/disable desktop left-eye mirror window."},
	{"desktop_mirror_active", reinterpret_cast<PyCFunction>(PyEngine_desktop_mirror_active), METH_NOARGS, "True if mirror window is open."},
	{"set_desktop_camera", reinterpret_cast<PyCFunction>(PyEngine_set_desktop_camera), METH_VARARGS, "Lock desktop cam (x,y,z,yaw,pitch)."},
	{"unlock_desktop_camera", reinterpret_cast<PyCFunction>(PyEngine_unlock_desktop_camera), METH_NOARGS, "Unlock interactive desktop camera."},
	{"set_avatar_visible", reinterpret_cast<PyCFunction>(PyEngine_set_avatar_visible), METH_VARARGS, "Show/hide torso+head when drawing hands."},
	{"set_arms_visible", reinterpret_cast<PyCFunction>(PyEngine_set_arms_visible), METH_VARARGS, "Show/hide arm bones; false = palm only."},
	{"save_desktop_screenshot", reinterpret_cast<PyCFunction>(PyEngine_save_desktop_screenshot), METH_VARARGS, "Save mirror frame as BMP path."},
	{"create_cube", reinterpret_cast<PyCFunction>(PyEngine_create_cube), METH_NOARGS, "Upload a unit cube mesh."},
	{"create_room", reinterpret_cast<PyCFunction>(PyEngine_create_room), METH_VARARGS, "Create a lightweight room mesh (floor at y=0)."},
	{"draw_room", reinterpret_cast<PyCFunction>(PyEngine_draw_room), METH_VARARGS, "Draw a room from create_room."},
	{"player_position", reinterpret_cast<PyCFunction>(PyEngine_player_position), METH_NOARGS, "World locomotion offset (x,y,z)."},
	{"player_velocity", reinterpret_cast<PyCFunction>(PyEngine_player_velocity), METH_NOARGS, "World locomotion velocity (x,y,z)."},
	{"create_hand", reinterpret_cast<PyCFunction>(PyEngine_create_hand), METH_VARARGS, "Create a procedural IK arm + gorilla hand."},
	{"draw_hand", reinterpret_cast<PyCFunction>(PyEngine_draw_hand), METH_VARARGS, "Draw IK arm to controller; optional close_fist."},
	{"load_mesh", reinterpret_cast<PyCFunction>(PyEngine_load_mesh), METH_VARARGS, "Upload a custom mesh."},
	{"load_obj", reinterpret_cast<PyCFunction>(PyEngine_load_obj), METH_VARARGS, "Load a Wavefront OBJ from disk."},
	{"draw_mesh", reinterpret_cast<PyCFunction>(PyEngine_draw_mesh), METH_VARARGS, "Queue one mesh draw."},
	{"submit_draws", reinterpret_cast<PyCFunction>(PyEngine_submit_draws), METH_VARARGS, "Queue many draws at once."},
	{"timings", reinterpret_cast<PyCFunction>(PyEngine_timings), METH_NOARGS, "Return (wait, record, submit, total) ms."},
	{"delta_time", reinterpret_cast<PyCFunction>(PyEngine_delta_time), METH_NOARGS, "Seconds since previous begin_frame."},
	{"backend_name", reinterpret_cast<PyCFunction>(PyEngine_backend_name), METH_NOARGS, "Vulkan/OpenXR runtime name."},
	{"controller_pose", reinterpret_cast<PyCFunction>(PyEngine_controller_pose), METH_VARARGS, "Grip pose matrix or None."},
	{"head_pose", reinterpret_cast<PyCFunction>(PyEngine_head_pose), METH_NOARGS, "Mid-eye HMD pose matrix or None."},
	{"button_down", reinterpret_cast<PyCFunction>(PyEngine_button_down), METH_VARARGS, "Controller button state."},
	{nullptr, nullptr, 0, nullptr},
};

static PyTypeObject PyEngineType = {
	PyVarObject_HEAD_INIT(nullptr, 0)
};

/**
 * Module init helper to finish PyTypeObject fields (MSVC-friendly).
 */
static void setup_engine_type() {
	PyEngineType.tp_name = "hypervr.Engine";
	PyEngineType.tp_basicsize = sizeof(PyEngineObject);
	PyEngineType.tp_dealloc = reinterpret_cast<destructor>(PyEngine_dealloc);
	PyEngineType.tp_flags = Py_TPFLAGS_DEFAULT;
	PyEngineType.tp_doc = "HyperVR immediate OpenXR/Vulkan engine (desktop_mode=True skips headset)";
	PyEngineType.tp_methods = PyEngine_methods;
	PyEngineType.tp_init = reinterpret_cast<initproc>(PyEngine_init);
	PyEngineType.tp_new = PyType_GenericNew;
}

static PyModuleDef module_def = {
	PyModuleDef_HEAD_INIT,
	"hypervr",
	"HyperVR — immediate-mode OpenXR/Vulkan renderer",
	-1,
	nullptr,
};

}  // namespace

PyMODINIT_FUNC PyInit_hypervr() {
	setup_engine_type();
	if (PyType_Ready(&PyEngineType) < 0) {
		return nullptr;
	}
	PyObject* module = PyModule_Create(&module_def);
	if (!module) {
		return nullptr;
	}
	Py_INCREF(&PyEngineType);
	if (PyModule_AddObject(module, "Engine", reinterpret_cast<PyObject*>(&PyEngineType)) < 0) {
		Py_DECREF(&PyEngineType);
		Py_DECREF(module);
		return nullptr;
	}
	PyModule_AddIntConstant(module, "HAND_LEFT", static_cast<int>(hypervr::Hand::Left));
	PyModule_AddIntConstant(module, "HAND_RIGHT", static_cast<int>(hypervr::Hand::Right));
	PyModule_AddIntConstant(module, "BUTTON_TRIGGER", static_cast<int>(hypervr::ControllerButton::Trigger));
	PyModule_AddIntConstant(module, "BUTTON_GRIP", static_cast<int>(hypervr::ControllerButton::Grip));
	PyModule_AddIntConstant(module, "BUTTON_MENU", static_cast<int>(hypervr::ControllerButton::Menu));
	PyModule_AddIntConstant(module, "BUTTON_A_X", static_cast<int>(hypervr::ControllerButton::A_X));
	PyModule_AddIntConstant(module, "BUTTON_B_Y", static_cast<int>(hypervr::ControllerButton::B_Y));
	return module;
}
