/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2019 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/

#include "voxelize.h"

#include "base/logging.h"
#include "base/metadata.h"

#include "stb_image.h"

#include "cubiquity.h"
#include "geometry.h"
#include "storage.h"
#include "voxelization.h"
#include "utility.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>
#include <list>
#include <cmath>
#include <queue>
#include <thread>
#include <mutex>
#include <unordered_set>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

// FIXME - Temporarily added to access writeing volume as images (should not be done from this file).
#include "commands/export/export.h"

using namespace Cubiquity;
using namespace Internals;

using namespace std;

void voxeliseSurface(Mesh& mesh, Volume& volume)
{
	mesh.build();

	MaterialId mainMaterial = findMainMaterial(mesh);

	Volume temp;
	voxelize(temp, mesh, mainMaterial, 0);
	volume.addVolume(temp);
	volume.bake();
	std::cout << std::endl;
}

bool voxelizeMesh(const std::filesystem::path& inputPath, Volume& volume, Metadata& metadata, float scaleFactor)
{
	/* ==== Configuration ==== */
	tinyobj::ObjReaderConfig objReaderConfig;
	objReaderConfig.triangulate = true;
	objReaderConfig.triangulation_method = "simple";
	objReaderConfig.vertex_color = false; // No default vertex colour.
	objReaderConfig.mtl_search_path = ""; // Same directory of .obj file


	/* ==== Parsing ==== */
	tinyobj::ObjReader objReader;
	if (!objReader.ParseFromFile(inputPath.string(), objReaderConfig)) {
		if (!objReader.Error().empty()) {
			std::cerr << std::endl << "Parser errors: " <<
				std::endl << objReader.Error() << std::endl; // Can hold multiple errors
		}
	}

	if (!objReader.Warning().empty()) {
		std::cout << std::endl << "Parser warnings: " <<
			std::endl << objReader.Warning() << std::endl; // Can hold multiple warnings
	}


	/* ==== Setup Materials ==== */
	const size_t max_obj_materials = 254; // Leave space for empty space and default material
	auto& obj_materials = objReader.GetMaterials();

	// Lack of materials isn't necessarily an problem (there may have been no 'mtllib' directive).
	// In other cases (missing file or materials) the parser will already have flagged a warning.
	if (obj_materials.empty()) {
		std::cout << "No materials found so a default material will be used." << std::endl;
	}

	if (obj_materials.size() > max_obj_materials) {
		std::cerr << "Too many materials found in .mtl file (found " << obj_materials.size()
			<< " but only " << max_obj_materials << " are supported)." << std::endl;
	}

	// The first material is always empty space, and is not used by any objects.
	metadata.materials.push_back(Metadata::EmptySpace);
	const int matBegin = 1; // Our real (non-empty) materials start here.

	// Copy as many materials as possible from the .mtl file.	
	const size_t materials_to_copy = std::min(obj_materials.size(), max_obj_materials);
	for (size_t i = 0; i < materials_to_copy; i++) {
		// Note - We can simplify with std::to_array from C++20 onwards.
		Material material = { obj_materials[i].name,
			{obj_materials[i].diffuse[0], obj_materials[i].diffuse[1], obj_materials[i].diffuse[2]} };
		metadata.materials.push_back(material);
	}

	// Default material is used by objects whose material was dropped because there were
	// too many materials, and by objects which don't actually have a material assigned.
	metadata.materials.push_back(Metadata::Default); // Last occupied material slot
	const int default_material = metadata.materials.size() - 1;
	

	/* ==== Process each shape in the file ==== */
	auto& attrib = objReader.GetAttrib();
	auto& shapes = objReader.GetShapes();
	for (size_t s = 0; s < shapes.size(); s++) {

		std::cout << "Processing '" << shapes[s].name << "' ("
			<< s + 1 << " of " << shapes.size() << ")..." << std::endl;
		bool log_material_warnings = true; // Used to supress (near) duplicate warnings.

		// If a shape contains multiple materials then we don't know which one to fill the interior
		// with. One option is to split the shape by material, but this is only appropriate if it
		// consists of multiple single-material components which each represent a closed surface. 
		// We don't have this knowledge so rely on such shapes being tagged via the name. This
		// provides a convenient alternative to splitting an object in a content creation package.
		const bool split_by_material = shapes[s].name.find("_split") != std::string::npos;
		if (split_by_material) {
			std::cout << "Note: The text '_split' in name '" << shapes[s].name <<
				"' causes the object to be split by material." << std::endl;
		}

		const bool is_thin = shapes[s].name.find("_thin") != std::string::npos;
		if (is_thin) {
			std::cout << "Note: The text '_thin' in name '" << shapes[s].name <<
				"' triggers special handling for thin objects." << std::endl;
		}

		Mesh mesh;
		mesh.name = shapes[s].name;
		mesh.isThin = is_thin;

		// Initialise current material from the first face.
		assert(shapes[s].mesh.material_ids.size() == shapes[s].mesh.num_face_vertices.size());
		if (shapes[s].mesh.material_ids.empty()) { continue; } // No faces in this object
		int currentMaterialId = shapes[s].mesh.material_ids[0]; // Valid due to above check

		// Loop over faces(polygon)
		size_t index_offset = 0;
		for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
			size_t fv = size_t(shapes[s].mesh.num_face_vertices[f]);

			Vector3f temp = { 0.0f, 0.0f, 0.0f };
			Triangle triangle(temp, temp, temp);

			// Loop over vertices in the face.
			for (size_t v = 0; v < fv; v++) {
				// access to vertex
				tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
				tinyobj::real_t vx = attrib.vertices[3 * size_t(idx.vertex_index) + 0];
				tinyobj::real_t vy = attrib.vertices[3 * size_t(idx.vertex_index) + 1];
				tinyobj::real_t vz = attrib.vertices[3 * size_t(idx.vertex_index) + 2];

				triangle.vertices[v] = Vector3f({ vx, vy, vz });

				// Check if `normal_index` is zero or positive. negative = no normal data
				if (idx.normal_index >= 0) {
					tinyobj::real_t nx = attrib.normals[3 * size_t(idx.normal_index) + 0];
					tinyobj::real_t ny = attrib.normals[3 * size_t(idx.normal_index) + 1];
					tinyobj::real_t nz = attrib.normals[3 * size_t(idx.normal_index) + 2];
				}

				// Check if `texcoord_index` is zero or positive. negative = no texcoord data
				if (idx.texcoord_index >= 0) {
					tinyobj::real_t tx = attrib.texcoords[2 * size_t(idx.texcoord_index) + 0];
					tinyobj::real_t ty = attrib.texcoords[2 * size_t(idx.texcoord_index) + 1];
				}

				// Optional: vertex colors
				// tinyobj::real_t red   = attrib.colors[3*size_t(idx.vertex_index)+0];
				// tinyobj::real_t green = attrib.colors[3*size_t(idx.vertex_index)+1];
				// tinyobj::real_t blue  = attrib.colors[3*size_t(idx.vertex_index)+2];
			}
			index_offset += fv;

			const int faceMaterial = shapes[s].mesh.material_ids[f];

			// If the material has changed then voxelise the last mesh and start a new one.
			if (split_by_material && (faceMaterial != currentMaterialId))
			{
				voxeliseSurface(mesh, volume);
				mesh = Mesh();
				mesh.name = shapes[s].name;
				mesh.isThin = is_thin;
				currentMaterialId = faceMaterial;
				log_material_warnings = true;
			}

			// Not all .obj files even include materials, so only warn if this one did.
			if ((faceMaterial < 0) && (obj_materials.size() > 0) && (log_material_warnings)) {
				std::cout << "Warning: Face " << f << " (and possibly others) of " << shapes[s].name << 
					" has no material assigned" << std::endl;
				log_material_warnings = false; // Supress similar warning for remaining faces.
			}


			if ((faceMaterial >= max_obj_materials) && (log_material_warnings)) {
				std::cout << "Warning: Face " << f << " (and possibly others) of " << shapes[s].name <<
					" has out-of-range material" << std::endl;
				log_material_warnings = false; // Supress similar warning for remaining faces.
			}

			// Use face material if valid, otherwise use default material.
			MaterialId matId = (faceMaterial >= 0) && (faceMaterial < max_obj_materials) ?
				static_cast<MaterialId>(matBegin + faceMaterial) : default_material;

			triangle.scale(scaleFactor);
			mesh.addTriangle(triangle, static_cast<MaterialId>(matId));
		}

		voxeliseSurface(mesh, volume);
	}

	return true;
}

bool voxelise(const flags::args& args)
{
	const std::filesystem::path inputPath(args.positional().at(1));
	const auto outputPath = args.get<std::filesystem::path>("output", "output.vol");
	const auto scaleFactor = args.get<float>("scale", 32.0);

	if (!std::filesystem::exists(inputPath))
	{
		cerr << "Path \'" << inputPath << "\' does not exist!" << endl;
		return false;
	}

	// Perform the voxelization
	Timer timer;
	Volume volume;
	Metadata metadata;

	std::string extension = inputPath.extension().string();

	if(extension == ".obj")
	{ 
		voxelizeMesh(inputPath, volume, metadata, scaleFactor);
	}
	else
	{
		log(Error, "Unrecognised extension \'", extension, "\'");
	}

	std::cout << "Voxelised in " << timer.elapsedTimeInSeconds() << " seconds" << std::endl;
	std::cout << "Node count before merging = " << volume.countNodes() << std::endl;

	// Save the result
	std::cout << "Saving volume as \'" << outputPath << "\'...";
	volume.save(outputPath.string());
	saveMetadataForVolume(metadata, outputPath);
	std::cout << "done." << std::endl;

	// FIXME - Temporary hack to automatically do image export after voxelisation.
	// In general the user should run cubiquity a second time to do the export.
	//saveVolumeAsImages(volume, metadata, ".");

	uint8 outside_material;
	int32 lower_x, lower_y, lower_z, upper_x, upper_y, upper_z;
	cubiquity_estimate_bounds(&volume, &outside_material, &lower_x, &lower_y, &lower_z, &upper_x, &upper_y, &upper_z);
	std::cout << Vector3i({ lower_x, lower_y, lower_z }) << " " << Vector3i({ upper_x, upper_y, upper_z }) << std::endl;

	int64_t histogram[256];
	cubiquity_compute_histogram(&volume, histogram);
	uint64 total = 0;
	for(int i = 0; i < 256; i++)
	{
		if (histogram[i] != 0) // Note that -1 can occur to indicate overflow
		{
			log(INF, "Material ", static_cast<uint16_t>(i), ": ", histogram[i], " voxels");
		}

		if (histogram[i] != -1) // FIXME - Handle overflow better
		{
			total += histogram[i];
		}
	}
	log(INF, "Total (non-overflowing): ", total, " voxels");

	return true;
}
