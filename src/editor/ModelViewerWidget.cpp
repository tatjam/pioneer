// Copyright © 2008-2023 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "ModelViewerWidget.h"
#include "EditorApp.h"
#include "NavLights.h"
#include "SDL_keycode.h"
#include "graphics/Graphics.h"
#include "graphics/Renderer.h"
#include "graphics/TextureBuilder.h"
#include "scenegraph/Animation.h"
#include "scenegraph/BinaryConverter.h"
#include "scenegraph/DumpVisitor.h"
#include "scenegraph/Loader.h"
#include "scenegraph/Model.h"
#include "scenegraph/Tag.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

using namespace Editor;

// ─── Utility Functions ───────────────────────────────────────────────────────

namespace {
	//azimuth/elevation in degrees to a dir vector
	vector3f az_el_to_dir(float yaw, float pitch)
	{
		//0,0 points to "right" (1,0,0)
		vector3f v;
		v.x = cos(DEG2RAD(yaw)) * cos(DEG2RAD(pitch));
		v.y = sin(DEG2RAD(pitch));
		v.z = sin(DEG2RAD(yaw)) * cos(DEG2RAD(pitch));
		return v;
	}
}

// ─── Setup ───────────────────────────────────────────────────────────────────

ModelViewerWidget::ModelViewerWidget(EditorApp *app) :
	ViewportWindow(app),
	m_bindings(app->GetInput()),
	m_input(app->GetInput()),
	m_renderer(app->GetRenderer()),
	m_options({})
{
	m_onModelChanged.connect(sigc::mem_fun(*this, &ModelViewerWidget::OnModelChanged));

	SetupInputAxes();
	ResetCamera();

	Graphics::MaterialDescriptor desc;

	//for grid, background
	Graphics::RenderStateDesc rsd;
	rsd.depthWrite = false;
	rsd.cullMode = Graphics::CULL_NONE;
	rsd.primitiveType = Graphics::TRIANGLES;
	m_bgMaterial.reset(m_renderer->CreateMaterial("vtxColor", desc, rsd));

	m_gridLines.reset(new Graphics::Drawables::GridLines(m_renderer));

	m_options.showGrid = true;
	m_options.gridInterval = 10.f;
}

ModelViewerWidget::~ModelViewerWidget()
{}

SceneGraph::Model *ModelViewerWidget::GetModel()
{
	return m_model.get();
}

void ModelViewerWidget::LoadModel(std::string_view path)
{
	ClearModel();

	try {
		if (ends_with_ci(path, ".sgm")) {
			//binary loader expects extension-less name. Might want to change this.
			std::string modelName = std::string(path.substr(0, path.size() - 4));
			SceneGraph::BinaryConverter bc(m_renderer);
			m_model.reset(bc.Load(modelName));
		} else {
			std::string modelName = std::string(path);
			SceneGraph::Loader loader(m_renderer, true, false);
			m_model.reset(loader.LoadModel(modelName));

			//dump warnings
			for (std::vector<std::string>::const_iterator it = loader.GetLogMessages().begin();
				 it != loader.GetLogMessages().end(); ++it) {
				Log::Warning("{}", *it);
			}
		}

		if (!m_model) {
			Log::Warning("Could not load model {}", path);
			return;
		}

		// Shields::ReparentShieldNodes(m_model.get());

		//set decal textures, max 4 supported.
		//Identical texture at the moment
		SetDecals("pioneer");

		// TODO: preload grid option from approximate model bounds
		m_options.gridInterval = 10.f;

		SceneGraph::DumpVisitor d(m_model.get());
		m_model->GetRoot()->Accept(d);
		Log::Verbose("{}", d.GetModelStatistics());

		// If we've got the tag_landing set then use it for an offset otherwise grab the AABB
		const SceneGraph::Tag *mt = m_model->FindTagByName("tag_landing");
		if (mt)
			m_landingMinOffset = mt->GetGlobalTransform().GetTranslate().y;
		else if (m_model->GetCollisionMesh())
			m_landingMinOffset = m_model->GetCollisionMesh()->GetAabb().min.y;
		else
			m_landingMinOffset = 0.0f;

		//note: stations won't demonstrate full docking light logic in MV
		m_navLights.reset(new NavLights(m_model.get()));
		m_navLights->SetEnabled(true);

		// m_shields.reset(new Shields(m_model.get()));
	} catch (SceneGraph::LoadingError &err) {
		// report the error and show model picker.
		m_model.reset();
		Log::Warning("Could not load model {}: {}", path, err.what());
	}

	if (m_model)
		m_onModelChanged.emit();
}

void ModelViewerWidget::ClearModel()
{
	m_model.reset();

	m_animations.clear();
	m_currentAnimation = nullptr;

	m_scaleModel.reset();

	m_options.mouselookEnabled = false;
	m_input->SetCapturingMouse(false);
	m_viewPos = vector3f(0.0f, 0.0f, 10.0f);
}

void ModelViewerWidget::OnModelChanged()
{
	ResetCamera();

	m_animations = m_model->GetAnimations();
	m_currentAnimation = m_animations.size() ? m_animations.front() : nullptr;
	// if (m_currentAnimation)
		// m_model->SetAnimationActive(0, true);
}

void ModelViewerWidget::CreateTestResources()
{
	//landingpad model for scale test
	SceneGraph::Loader loader(m_renderer);
	try {
		SceneGraph::Model *m = loader.LoadModel("scale");
		m_scaleModel.reset(m);
	} catch (SceneGraph::LoadingError &) {
		Log::Warning("Could not load scale model");
	}
}

void ModelViewerWidget::SetDecals(std::string_view texname)
{
	if (!m_model) return;

	std::string path = fmt::format("textures/decals/{}.dds", texname);

	m_decalTexture = Graphics::TextureBuilder::Decal(path).GetOrCreateTexture(m_renderer, "decal");

	m_model->SetDecalTexture(m_decalTexture, 0);
	m_model->SetDecalTexture(m_decalTexture, 1);
	m_model->SetDecalTexture(m_decalTexture, 2);
	m_model->SetDecalTexture(m_decalTexture, 3);
}

const char *ModelViewerWidget::GetWindowName()
{
	// if (m_model) {
	// 	return m_model->GetName().c_str();
	// } else {
	// 	return "Model Viewer";
	// }

	return "Model Viewer";
}

// ─── Input Handling ──────────────────────────────────────────────────────────

void ModelViewerWidget::SetupInputAxes()
{
	auto *page = m_input->GetBindingPage("ModelViewer");
	auto *group = page->GetBindingGroup("View");

	// Don't add this to REGISTER_INPUT_BINDING because these bindings aren't used by the game
#define AXIS(val, name, axis, positive, negative)                                                \
	m_input->AddAxisBinding(name, group, InputBindings::Axis(axis, { positive }, { negative })); \
	m_bindings.val = m_bindings.AddAxis(name)

#define ACTION(val, name, b1, b2)                                                  \
	m_input->AddActionBinding(name, group, InputBindings::Action({ b1 }, { b2 })); \
	m_bindings.val = m_bindings.AddAction(name)

	AXIS(zoomAxis, "BindZoomAxis", {}, SDLK_EQUALS, SDLK_MINUS);

	AXIS(moveForward, "BindMoveForward", {}, SDLK_w, SDLK_s);
	AXIS(moveLeft, "BindMoveLeft", {}, SDLK_a, SDLK_d);
	AXIS(moveUp, "BindMoveUp", {}, SDLK_q, SDLK_e);

	// Like Blender, but a bit different because we like that
	// 1 - front (+ctrl back)
	// 7 - top (+ctrl bottom)
	// 3 - left (+ctrl right)
	// 2,4,6,8 incrementally rotate

	ACTION(viewFront, "BindViewFront", SDLK_KP_1, SDLK_m);
	m_bindings.viewFront->onPressed.connect([=]() {
		this->ChangeCameraPreset(m_input->KeyModState() & KMOD_CTRL ? CameraPreset::Back : CameraPreset::Front);
	});

	ACTION(viewLeft, "BindViewLeft", SDLK_KP_3, SDLK_PERIOD);
	m_bindings.viewLeft->onPressed.connect([=]() {
		this->ChangeCameraPreset(m_input->KeyModState() & KMOD_CTRL ? CameraPreset::Right : CameraPreset::Left);
	});

	ACTION(viewTop, "BindViewTop", SDLK_KP_7, SDLK_u);
	m_bindings.viewTop->onPressed.connect([=]() {
		this->ChangeCameraPreset(m_input->KeyModState() & KMOD_CTRL ? CameraPreset::Bottom : CameraPreset::Top);
	});

	AXIS(rotateViewLeft, "BindRotateViewLeft", {}, SDLK_KP_6, SDLK_KP_4);
	AXIS(rotateViewUp, "BindRotateViewUp", {}, SDLK_KP_8, SDLK_KP_2);

#undef AXIS
#undef ACTION
}

void ModelViewerWidget::OnAppearing()
{
	m_input->AddInputFrame(&m_bindings);
}

void ModelViewerWidget::OnDisappearing()
{
	m_input->RemoveInputFrame(&m_bindings);
}

void ModelViewerWidget::OnHandleInput(bool clicked, bool released, ImVec2 mousePos)
{
	if (m_input->IsKeyPressed(SDLK_SPACE)) {
		ResetCamera();
	}

	if (m_input->IsKeyPressed(SDLK_o))
		m_options.orthoView = !m_options.orthoView;

	if (m_input->IsKeyPressed(SDLK_z))
		m_options.wireframe = !m_options.wireframe;

	if (m_input->IsKeyPressed(SDLK_f))
		ToggleViewControlMode();

	if (!released) {
		HandleCameraInput(GetApp()->DeltaTime());
	}
}

void ModelViewerWidget::HandleCameraInput(float deltaTime)
{
	static const float BASE_ZOOM_RATE = 1.0f / 12.0f;
	float zoomRate = (BASE_ZOOM_RATE * 8.0f) * deltaTime;
	float rotateRate = 25.f * deltaTime;
	float moveRate = 10.0f * deltaTime;

	bool isShiftPressed = m_input->KeyState(SDLK_LSHIFT);

	if (isShiftPressed) {
		zoomRate *= 8.0f;
		moveRate *= 4.0f;
		rotateRate *= 4.0f;
	}

	std::array<int, 2> mouseMotion;
	m_input->GetMouseMotion(mouseMotion.data());

	bool rightMouseDown = m_input->MouseButtonState(SDL_BUTTON_RIGHT);

	if (m_options.mouselookEnabled) {
		const float degrees_per_pixel = 0.2f;
		if (!rightMouseDown) {
			// yaw and pitch
			const float rot_y = degrees_per_pixel * mouseMotion[0];
			const float rot_x = degrees_per_pixel * mouseMotion[1];
			const matrix3x3f rot =
				matrix3x3f::RotateX(DEG2RAD(rot_x)) *
				matrix3x3f::RotateY(DEG2RAD(rot_y));

			m_viewRot = m_viewRot * rot;
		} else {
			// roll
			m_viewRot = m_viewRot * matrix3x3f::RotateZ(DEG2RAD(degrees_per_pixel * mouseMotion[0]));
		}

		vector3f motion(
			m_bindings.moveLeft->GetValue(),
			m_bindings.moveUp->GetValue(),
			m_bindings.moveForward->GetValue());

		m_viewPos += m_viewRot * motion;
	} else {
		//zoom
		m_zoom += m_bindings.zoomAxis->GetValue() * BASE_ZOOM_RATE;

		//zoom with mouse wheel
		int mouseWheel = m_input->GetMouseWheel();
		if (mouseWheel) m_zoom += mouseWheel > 0 ? -BASE_ZOOM_RATE : BASE_ZOOM_RATE;

		m_zoom = Clamp(m_zoom, -10.0f, 10.0f); // distance range: [baseDistance * 1/1024, baseDistance * 1024]

		//rotate

		if (m_input->IsKeyDown(SDLK_UP)) m_rot.x += rotateRate;
		if (m_input->IsKeyDown(SDLK_DOWN)) m_rot.x -= rotateRate;
		if (m_input->IsKeyDown(SDLK_LEFT)) m_rot.y += rotateRate;
		if (m_input->IsKeyDown(SDLK_RIGHT)) m_rot.y -= rotateRate;

		m_rot.x += rotateRate * m_bindings.rotateViewLeft->GetValue();
		m_rot.y += rotateRate * -m_bindings.rotateViewUp->GetValue();

		//mouse rotate when right button held
		if (rightMouseDown) {
			m_rot.y += 0.2f * mouseMotion[0];
			m_rot.x += 0.2f * mouseMotion[1];
		}
	}
}

void ModelViewerWidget::ResetCamera()
{
	m_baseDistance = m_model ? m_model->GetDrawClipRadius() * 1.5f : 100.f;
	m_gridDistance = m_model ? m_model->GetDrawClipRadius() : 100.f;
	m_rot = { 30.f, 45.f };
	m_zoom = 0.f;
}

void ModelViewerWidget::ChangeCameraPreset(CameraPreset preset)
{
	if (!m_model) return;

	switch (preset) {
	case CameraPreset::Bottom:
		m_rot.x = -90.0f;
		m_rot.y = 0.0f;
		break;
	case CameraPreset::Top:
		m_rot.x = 90.0f;
		m_rot.y = 0.0f;
		break;

	case CameraPreset::Left:
		m_rot.x = 0.f;
		m_rot.y = 90.0f;
		break;
	case CameraPreset::Right:
		m_rot.x = 0.f;
		m_rot.y = -90.0f;
		break;

	case CameraPreset::Front:
		m_rot.x = 0.f;
		m_rot.y = 180.0f;
		break;
	case CameraPreset::Back:
		m_rot.x = 0.f;
		m_rot.y = 0.0f;
		break;
	}
}

void ModelViewerWidget::ToggleViewControlMode()
{
	m_options.mouselookEnabled = !m_options.mouselookEnabled;
	m_input->SetCapturingMouse(m_options.mouselookEnabled);

	if (m_options.mouselookEnabled) {
		m_viewRot = matrix3x3f::RotateY(DEG2RAD(m_rot.y)) * matrix3x3f::RotateX(DEG2RAD(Clamp(m_rot.x, -90.0f, 90.0f)));
		m_viewPos = (m_baseDistance * powf(2.0f, m_zoom)) * m_viewRot.VectorZ();
	} else {
		// TODO: re-initialise the turntable style view position from the current mouselook view
		ResetCamera();
	}
}

void ModelViewerWidget::OnUpdate(float deltaTime)
{
	if (m_model) {

		// Update navlights
		m_navLights->Update(GetApp()->DeltaTime());

		// Update animation playback
		if (m_currentAnimation) {

			if (m_model->GetAnimationActive(m_model->FindAnimationIndex(m_currentAnimation))) {
				double progress = m_currentAnimation->GetProgress() + GetApp()->DeltaTime() / m_currentAnimation->GetDuration();
				m_currentAnimation->SetProgress(fmod(progress, 1.0));

				m_currentAnimation->Interpolate();
			}

		}

	}
}

// ─── Model Rendering ─────────────────────────────────────────────────────────

void ModelViewerWidget::OnRender(Graphics::Renderer *r)
{
	m_renderer = r;

	// render the gradient backdrop
	DrawBackground();

	// Setup for 3d drawing
	UpdateLights();
	UpdateCamera();

	// Render the active model
	if (m_model) {
		DrawModel(m_modelViewMat);
	}

	// Render any extra effects
	PostRender();

	// helper rendering
	if (m_options.showLandingPad) {
		if (!m_scaleModel)
			CreateTestResources();
		m_scaleModel->Render(m_modelViewMat * matrix4x4f::Translation(0.f, m_landingMinOffset, 0.f));
	}

	if (m_options.showGrid) {
		DrawGrid(r, m_gridDistance);
	}
}

void ModelViewerWidget::DrawBackground()
{
	m_renderer->SetOrthographicProjection(0.f, 1.f, 0.f, 1.f, 0.f, 1.f);
	m_renderer->SetTransform(matrix4x4f::Identity());

	if (!m_bgMesh) {
		const Color top = Color::BLACK;
		const Color bottom = Color(28, 31, 36);
		Graphics::VertexArray bgArr(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_DIFFUSE, 6);
		// triangle 1
		bgArr.Add(vector3f(0.f, 0.f, 0.f), bottom);
		bgArr.Add(vector3f(1.f, 0.f, 0.f), bottom);
		bgArr.Add(vector3f(1.f, 1.f, 0.f), top);
		// triangle 2
		bgArr.Add(vector3f(0.f, 0.f, 0.f), bottom);
		bgArr.Add(vector3f(1.f, 1.f, 0.f), top);
		bgArr.Add(vector3f(0.f, 1.f, 0.f), top);

		m_bgMesh.reset(m_renderer->CreateMeshObjectFromArray(&bgArr));
	}

	m_renderer->DrawMesh(m_bgMesh.get(), m_bgMaterial.get());
}

void ModelViewerWidget::UpdateCamera()
{
	Graphics::ViewportExtents extents = GetViewportExtents();

	m_renderer->SetTransform(matrix4x4f::Identity());

	// setup rendering
	if (!m_options.orthoView) {
		m_renderer->SetPerspectiveProjection(85, float(extents.w) / float(extents.h), 0.1f, 100000.f);
	} else {
		/* TODO: Zoom in ortho mode seems don't work as in perspective mode,
			/ I change "screen dimensions" to avoid the problem.
			/ However the zoom needs more care
		*/
		if (m_zoom <= 0.0) m_zoom = 0.01;
		float screenW = extents.w * m_zoom / 10.f;
		float screenH = extents.h * m_zoom / 10.f;
		matrix4x4f orthoMat = matrix4x4f::OrthoMatrix(screenW, screenH, 0.1f, 100000.0f);
		m_renderer->SetProjection(orthoMat);
	}

	// calc camera info
	float zd = 0;
	if (m_options.mouselookEnabled) {
		m_modelViewMat = m_viewRot.Transpose() * matrix4x4f::Translation(-m_viewPos);
	} else {
		m_rot.x = Clamp(m_rot.x, -90.0f, 90.0f);
		matrix4x4f rot = matrix4x4f::Identity();
		rot.RotateX(DEG2RAD(-m_rot.x));
		rot.RotateY(DEG2RAD(-m_rot.y));
		if (m_options.orthoView)
			zd = -m_baseDistance;
		else
			zd = -(m_baseDistance * powf(2.0f, m_zoom));
		m_modelViewMat = matrix4x4f::Translation(0.0f, 0.0f, zd) * rot;
	}
}

void ModelViewerWidget::DrawModel(matrix4x4f trans)
{
	assert(m_model);

	m_model->UpdateAnimations();

	// this causes all debug visuals to be re-generated each frame, useful when scrubbing animations
	// also a good incentive to make your debug visuals *fast*
	m_model->SetDebugFlags(
		(m_options.showAabb ? SceneGraph::Model::DEBUG_BBOX : 0x0) |
		(m_options.showCollMesh ? SceneGraph::Model::DEBUG_COLLMESH : 0x0) |
		(m_options.showTags ? SceneGraph::Model::DEBUG_TAGS : 0x0) |
		(m_options.showDockingLocators ? SceneGraph::Model::DEBUG_DOCKING : 0x0) |
		(m_options.showGeomBBox ? SceneGraph::Model::DEBUG_GEOMBBOX : 0x0) |
		(m_options.wireframe ? SceneGraph::Model::DEBUG_WIREFRAME : 0x0));

	m_model->Render(m_modelViewMat);
	m_navLights->Render(m_renderer);
}

void ModelViewerWidget::DrawGrid(Graphics::Renderer *r, float clipRadius)
{
	const float max = powf(10, ceilf(log10f(clipRadius * 1.1)));

	r->SetTransform(m_modelViewMat);
	m_gridLines->Draw(r, { max, max }, m_options.gridInterval);

	if (m_options.showVerticalGrids) {
		r->SetTransform(m_modelViewMat * matrix4x4f::RotateXMatrix(M_PI * 0.5));
		m_gridLines->Draw(r, { max, max }, m_options.gridInterval);

		r->SetTransform(m_modelViewMat * matrix4x4f::RotateZMatrix(M_PI * 0.5));
		m_gridLines->Draw(r, { max, max }, m_options.gridInterval);
	}

	// industry-standard red/green/blue XYZ axis indicator
	r->SetTransform(m_modelViewMat * matrix4x4f::ScaleMatrix(clipRadius));
	Graphics::Drawables::GetAxes3DDrawable(r)->Draw(r);
}


void ModelViewerWidget::UpdateLights()
{
	using Graphics::Light;
	std::vector<Light> lights;

	switch (m_options.lightPreset) {
	case 0:
	default:
		//Front white
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(90, 0), Color::WHITE, Color::WHITE));
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(0, -90), Color(13, 13, 26), Color::WHITE));
		break;
	case 1:
		//Two-point
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(120, 0), Color(230, 204, 204), Color::WHITE));
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(-30, -90), Color(178, 128, 0), Color::WHITE));
		break;
	case 2:
		//Backlight
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(-75, 20), Color::WHITE, Color::WHITE));
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(0, -90), Color(13, 13, 26), Color::WHITE));
		break;
	case 3:
		//4 lights
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(0, 90), Color::YELLOW, Color::WHITE));
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(0, -90), Color::GREEN, Color::WHITE));
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(0, 45), Color::BLUE, Color::WHITE));
		lights.push_back(Light(Light::LIGHT_DIRECTIONAL, az_el_to_dir(0, -45), Color::WHITE, Color::WHITE));
		break;
	};

	m_renderer->SetLights(int(lights.size()), &lights[0]);
}

// ─── Draw Overlays ───────────────────────────────────────────────────────────

namespace ImGui {

	bool MenuButton(const char *label)
	{
		ImVec2 screenPos = ImGui::GetCursorScreenPos();

		if (ImGui::Button(label))
			ImGui::OpenPopup(label);

		if (ImGui::IsPopupOpen(label)) {
			ImGuiPopupFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavFocus;
			ImGui::SetNextWindowPos(screenPos + ImVec2(0.f, ImGui::GetFrameHeightWithSpacing()));

			return ImGui::BeginPopup(label, flags);
		}

		return false;
	}

	bool ToggleIconButton(const char *icon, bool *value, ImVec4 activeColor)
	{
		if (*value)
			ImGui::PushStyleColor(ImGuiCol_Button, activeColor);

		bool changed = ImGui::Button(icon);

		if (*value)
			ImGui::PopStyleColor(1);

		if (changed)
			*value = !*value;

		return changed;
	}

}

void ModelViewerWidget::OnDraw()
{
	if (m_options.hideUI) {
		return;
	}

	ImVec2 cursorPos = ImGui::GetCursorPos();

	if (ImGui::MenuButton("Options")) {
		ImGui::Checkbox("Show Scale Model", &m_options.showLandingPad);
		ImGui::Checkbox("Show Collision Mesh", &m_options.showCollMesh);
		m_options.showAabb = m_options.showCollMesh;
		ImGui::Checkbox("Show Geometry Bounds", &m_options.showGeomBBox);
		ImGui::Checkbox("Show Tags", &m_options.showTags);
		m_options.showDockingLocators = m_options.showTags;

		ImGui::EndMenu();
	}

	ImGui::Separator();

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
	ImGui::ToggleIconButton("#", &m_options.showGrid, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
	ImGui::PopStyleVar(1);

	float width = ImGui::CalcTextSize("1000m").x + ImGui::GetFrameHeightWithSpacing();
	ImGui::SetNextItemWidth(width);

	std::string currentGridMode = std::to_string(int(m_options.gridInterval)) + "m";
	if (ImGui::BeginCombo("##Grid Mode", currentGridMode.c_str())) {
		if (ImGui::Selectable("1m"))
			m_options.gridInterval = 1.0f;

		if (ImGui::Selectable("10m"))
			m_options.gridInterval = 10.0f;

		if (ImGui::Selectable("100m"))
			m_options.gridInterval = 100.0f;

		if (ImGui::Selectable("1000m"))
			m_options.gridInterval = 1000.0f;

		ImGui::EndCombo();
	}

	if (m_animations.empty()) {
		return;
	}

	uint32_t animIndex = m_model->FindAnimationIndex(m_currentAnimation);
	bool animActive = m_model->GetAnimationActive(animIndex);

	float frameHeight = ImGui::GetFrameHeight();
	float bottomPosOffset = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
	ImGui::SetCursorPos(cursorPos + ImVec2(0.f, bottomPosOffset));

	const char *play_pause = animActive ? "||###Play/Pause" : ">###Play/Pause";

	if (ImGui::Button(play_pause, ImVec2(frameHeight, frameHeight))) {
		m_model->SetAnimationActive(animIndex, !animActive);
	}

	float progress = m_currentAnimation->GetProgress();

	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x);
	if (ImGui::SliderFloat("##AnimProgress", &progress, 0.f, 1.f)) {
		m_currentAnimation->SetProgress(progress);
		m_currentAnimation->Interpolate();
	}
}
