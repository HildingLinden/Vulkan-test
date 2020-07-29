#include "engine.h"
#include "shapes.h"
#include "physicsSystem.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>

#include <glm/gtc/matrix_transform.hpp>

class FPSCounter {
private:
	std::chrono::time_point<std::chrono::high_resolution_clock> time = std::chrono::high_resolution_clock::now();
	float fpsUpdateInterval;
	float fpsUpdateTimer = 0.0f;
	int frameCount = 0;
	std::string title;
	Engine &app;
public:
	FPSCounter(std::string title, Engine &app, float interval) : title(title), app(app), fpsUpdateInterval(interval) {}

	float getTime() {
		std::chrono::time_point<std::chrono::high_resolution_clock> currentTime = std::chrono::high_resolution_clock::now();

		std::chrono::duration<float> elapsedTime = currentTime - time;
		time = currentTime;

		fpsUpdateTimer += elapsedTime.count();
		frameCount++;

		if (fpsUpdateTimer > fpsUpdateInterval) {
			int fps = round(frameCount / fpsUpdateTimer);
			app.changeTitle(title + std::to_string(fps) + " fps - ");
			fpsUpdateTimer = 0.0f;
			frameCount = 0;
		}

		return elapsedTime.count();
	}
};

std::vector<Rect> rects;

const uint32_t WIDTH = 1280;
const uint32_t HEIGHT = 720;

const bool LIMIT_FPS = false;

int main() {
	try {
		// Initialization
		std::string title("Vulkan Test Application - ");

		Engine app(WIDTH, HEIGHT, title, ShaderBufferType::SSBO);
		app.init();		

		FPSCounter fpsCounter(title, app, 1.0f);

		PhysicsSystem physics;

		// Seeding the RNG
		srand((unsigned)time(NULL));

		// Creating some static rectangles
		std::vector<Rect> tmpRects;
		for (size_t i = 0; i < 50; i++) {
			for (size_t j = 0; j < 10; j++) {
				Rect r(100 + i * 2 + j * 100, 100 + j * 10, 2, 2);
				tmpRects.push_back(r);
			}
		}
		Rect r(10, 10, 10, 10, false);
		tmpRects.push_back(r);

		// Test if all new rects can be added to the graphics engine if so add them to rects
		if (app.addRects(tmpRects)) {
			rects.insert(std::end(rects), std::begin(tmpRects), std::end(tmpRects));
			physics.updateObjects(rects);
		}

		// Main game loop
		while (!app.shouldClose()) {
			float elapsedTime = fpsCounter.getTime();

			// Limit fps
			if (LIMIT_FPS && elapsedTime < 0.01) {
				std::this_thread::sleep_for(std::chrono::microseconds(10000 - (long)(elapsedTime * 1000000)));
				elapsedTime += fpsCounter.getTime();
			}

			uint32_t currentWidth = app.getWidth();
			uint32_t currentHeight = app.getHeight();

			physics.setScreenBB(currentWidth, currentHeight);

			// User input
			app.pollEvents();
			if (app.checkKeyPress(GLFW_KEY_RIGHT)) {
				for (Rect& rect : rects) {
					rect.vel[0] += 500 * elapsedTime;
				}
			}
			if (app.checkKeyPress(GLFW_KEY_LEFT)) {
				for (Rect& rect : rects) {
					rect.vel[0] -= 500 * elapsedTime;
				}
			}
			if (app.checkKeyPress(GLFW_KEY_UP)) {
				for (Rect& rect : rects) {
					if (rect.canJump) {
						rect.vel[1] -= 250;
						rect.canJump = false;
					}
				}
			}
/*			if (app.checkMouseClick()) {
				std::vector<Rect> tmpRects;
				for (size_t i = 0; i < 1; i++) {
					Rect r(rand() % (currentWidth-10), rand() % (currentHeight-10), 10, 10, false);
					tmpRects.push_back(r);
				}
				if (app.addRects(tmpRects)) {
					rects.insert(std::end(rects), std::begin(tmpRects), std::end(tmpRects));
					physics.updateObjects(rects);
				}
			}*/			

			if (rects.size() > 0) {
				// Physics
				//#pragma omp parallel for
				physics.update(elapsedTime);

				// Render
				app.drawFrame();
			}
		}

		// Clean up
		app.cleanup();
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return 1;
	}

	return 0;
}