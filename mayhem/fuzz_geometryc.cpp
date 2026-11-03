// Fuzzes geometryc's mesh importers: the Wavefront OBJ parser (parseObj) and the glTF/GLB importer
// (cgltf_parse + processGltfNode). geometryc.cpp is included as a translation unit with its main()
// renamed, so the real upstream code is exercised without editing it.
// First input byte: bit 0 = glTF instead of OBJ, bit 1 = hasBc. The rest is the file.
#define main geometryc_main
#include "../tools/geometryc/geometryc.cpp"
#undef main

#include <stdlib.h>

namespace
{
	// External glTF buffers resolve to a fixed in-memory blob instead of touching the filesystem.
	cgltf_result blobRead(const cgltf_memory_options*, const cgltf_file_options*, const char*, cgltf_size* _size, void** _data)
	{
		const cgltf_size size = 1 << 16;
		uint8_t* blob = (uint8_t*)malloc(size);
		for (cgltf_size ii = 0; ii < size; ++ii)
		{
			blob[ii] = uint8_t(ii * 7);
		}
		*_size = size;
		*_data = blob;
		return cgltf_result_success;
	}

	void blobRelease(const cgltf_memory_options*, const cgltf_file_options*, void* _data, cgltf_size)
	{
		free(_data);
	}

	void importGltf(const uint8_t* _data, size_t _size, Mesh* _mesh, bool _hasBc)
	{
		_mesh->m_coordinateSystem.m_handedness = bx::Handedness::Right;
		_mesh->m_coordinateSystem.m_forward    = Axis::PositiveZ;
		_mesh->m_coordinateSystem.m_up         = Axis::PositiveY;

		Group group;
		group.m_startTriangle = 0;
		group.m_numTriangles  = 0;

		cgltf_options options = { };
		options.file.read    = blobRead;
		options.file.release = blobRelease;

		cgltf_data* data = NULL;
		if (cgltf_result_success != cgltf_parse(&options, _data, _size, &data) )
		{
			return;
		}

		if (cgltf_result_success == cgltf_load_buffers(&options, data, "") )
		{
			for (cgltf_size sceneIndex = 0; sceneIndex < data->scenes_count; ++sceneIndex)
			{
				cgltf_scene* scene = &data->scenes[sceneIndex];
				for (cgltf_size nodeIndex = 0; nodeIndex < scene->nodes_count; ++nodeIndex)
				{
					processGltfNode(scene->nodes[nodeIndex], _mesh, &group, _hasBc);
				}
			}
		}

		cgltf_free(data);
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* _data, size_t _size)
{
	if (_size < 2 || _size > 1 << 20)
	{
		return -1;
	}

	const bool isGltf = 0 != (_data[0] & 1);
	const bool hasBc  = 0 != (_data[0] & 2);
	_data += 1;
	_size -= 1;

	Mesh mesh;
	if (isGltf)
	{
		importGltf(_data, _size, &mesh, hasBc);
	}
	else
	{
		char* text = new char[_size + 1];
		bx::memCopy(text, _data, _size);
		text[_size] = '\0';
		parseObj(text, uint32_t(_size), &mesh, hasBc);
		delete [] text;
	}

	return 0;
}
