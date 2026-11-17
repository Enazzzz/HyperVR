#include "hypervr/mesh_loader.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hypervr {

void load_obj_file(
	const std::string& path,
	std::vector<Vertex>& out_vertices,
	std::vector<std::uint32_t>& out_indices,
	float r,
	float g,
	float b) {
	std::ifstream in(path);
	if (!in) {
		throw std::runtime_error("Failed to open OBJ: " + path);
	}

	std::vector<float> positions;
	std::vector<std::uint32_t> faces;
	out_vertices.clear();
	out_indices.clear();

	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream ss(line);
		std::string tag;
		ss >> tag;
		if (tag == "v") {
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			ss >> x >> y >> z;
			positions.push_back(x);
			positions.push_back(y);
			positions.push_back(z);
		} else if (tag == "f") {
			std::vector<std::uint32_t> face;
			std::string token;
			while (ss >> token) {
				const std::size_t slash = token.find('/');
				const std::string index_text = slash == std::string::npos ? token : token.substr(0, slash);
				int idx = std::stoi(index_text);
				if (idx < 0) {
					idx = static_cast<int>(positions.size() / 3) + idx + 1;
				}
				if (idx <= 0) {
					throw std::runtime_error("Invalid OBJ face index in " + path);
				}
				face.push_back(static_cast<std::uint32_t>(idx - 1));
			}
			if (face.size() < 3) {
				continue;
			}
			for (std::size_t i = 1; i + 1 < face.size(); ++i) {
				faces.push_back(face[0]);
				faces.push_back(face[i]);
				faces.push_back(face[i + 1]);
			}
		}
	}

	const std::size_t vert_count = positions.size() / 3;
	if (vert_count == 0 || faces.empty()) {
		throw std::runtime_error("OBJ contained no triangles: " + path);
	}

	out_vertices.resize(vert_count);
	for (std::size_t i = 0; i < vert_count; ++i) {
		Vertex& v = out_vertices[i];
		v.px = positions[i * 3 + 0];
		v.py = positions[i * 3 + 1];
		v.pz = positions[i * 3 + 2];
		v.r = r;
		v.g = g;
		v.b = b;
	}
	out_indices = std::move(faces);

	for (std::uint32_t idx : out_indices) {
		if (idx >= vert_count) {
			throw std::runtime_error("OBJ face index out of range in " + path);
		}
	}
}

}  // namespace hypervr
