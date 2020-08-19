#pragma once

#include <cstdint>
#include <vector>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

class Rect {
public:
	float x, y;
	float width, height;
	glm::vec2 vel = glm::vec2(0.0f);

	bool isStatic;
	bool canJump = false;

	glm::mat4 *modelMatrix = new glm::mat4(1.0f);

	std::string texture;

	Rect(float x, float y, float width, float height, std::string texture, bool isStatic = true);
	void update();
};