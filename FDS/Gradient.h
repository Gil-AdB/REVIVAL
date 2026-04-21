#pragma once

#include <vector>

struct GradientEndpoint {
	// input is in range [0, 1]
	float u = 0.0;
	Color c = {0.0, 0.0, 0.0, 0.0};

	GradientEndpoint(float u, Color c) : u(u), c(c) {}
};

Material* Generate_Gradient(const std::vector<GradientEndpoint>& endpoints, int txSize, float vSlack, bool rainDrop);
