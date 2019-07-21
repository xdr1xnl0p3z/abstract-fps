/** \file App.cpp */
#include "App.h"
#include "TargetEntity.h"
#include "PlayerEntity.h"

// Scale and offset for target
const float App::TARGET_MODEL_ARRAY_SCALING = 0.2f;
const float App::TARGET_MODEL_ARRAY_OFFSET = 40;

/** global startup config - sets playMode and experiment/user paths */
StartupConfig startupConfig;

App::App(const GApp::Settings& settings) : GApp(settings) {}

/** Initialize the app */
void App::onInit() {
	// Seed random based on the time
	Random::common().reset(uint32(time(0)));

	// Initialize the app
	GApp::onInit();

	// Load per user settings from file
	userTable = UserTable::load(startupConfig.userConfig());
	userTable.printToLog();

	// Load per experiment user settings from file
	userStatusTable = UserStatusTable::load();
	userStatusTable.printToLog();

	// Load experiment setting from file
	experimentConfig = ExperimentConfig::load(startupConfig.experimentConfig());
	experimentConfig.printToLog();

	// Get and save system configuration
	SystemConfig sysConfig = SystemConfig::load();
	sysConfig.printToLog();											// Print system info to log.txt
	sysConfig.toAny().save("systemconfig.Any");						// Update the any file here (new system info to write)

	// Setup the display mode
	setSubmitToDisplayMode(
		//SubmitToDisplayMode::EXPLICIT);
		SubmitToDisplayMode::MINIMIZE_LATENCY);
		//SubmitToDisplayMode::BALANCE);
	    //SubmitToDisplayMode::MAXIMIZE_THROUGHPUT);

	// Setup the scene
	m_scene = PhysicsScene::create(m_ambientOcclusion);
	setScene(m_scene);
	scene()->registerEntitySubclass("PlayerEntity", &PlayerEntity::create);			// Register the player entity for creation
	scene()->registerEntitySubclass("FlyingEntity", &FlyingEntity::create);			// Create a target

	// Setup the GUI
	showRenderingStats = false;
	makeGUI();
	developerWindow->videoRecordDialog->setCaptureGui(true);
	   
	// Load fonts and images
	outputFont = GFont::fromFile(System::findDataFile("arial.fnt"));
	hudFont = GFont::fromFile(System::findDataFile(experimentConfig.hudFont));
	m_combatFont = GFont::fromFile(System::findDataFile(experimentConfig.combatTextFont));
	hudTexture = Texture::fromFile(System::findDataFile("gui/hud.png"));

	// Check for play mode specific parameters
	if (startupConfig.playMode) {
		m_fireSound = Sound::create(System::findDataFile(experimentConfig.weapon.fireSound));
		m_explosionSound = Sound::create(System::findDataFile(experimentConfig.explosionSound));
	}

	// Load models and set the reticle
	loadModels();
	setReticle(m_reticleIndex);

	// Create a series of colored materials to choose from for target health
	for (int i = 0; i < m_MatTableSize; i++) {
		float complete = (float)i/ m_MatTableSize;
		Color3 color = experimentConfig.targetHealthColors[0]*complete + experimentConfig.targetHealthColors[1]*(1.0f-complete);
		UniversalMaterial::Specification materialSpecification;
		materialSpecification.setLambertian(Texture::Specification(color));
		materialSpecification.setEmissive(Texture::Specification(color * 0.7f));
		materialSpecification.setGlossy(Texture::Specification(Color4(0.4f, 0.2f, 0.1f, 0.8f)));
		m_materials.append(UniversalMaterial::create(materialSpecification));
	}

	updateMouseSensitivity();			// Update (apply) mouse sensitivity
	updateSessionDropDown();			// Update the session drop down to remove already completed sessions
	updateSessionPress();				// Update session to create results file/start collection
}

/** Handle then user settings window visibility */
void App::openUserSettingsWindow() {
    m_userSettingsMode = true;
    m_userSettingsWindow->setVisible(m_userSettingsMode);
}

/** Update the mouse mode/sensitivity */
void App::updateMouseSensitivity() {
    // G3D expects mouse sensitivity in radians
    // we're converting from mouseDPI and centimeters/360 which explains
    // the screen resolution (dots), cm->in factor (2.54) and 2PI
    double mouseSensitivity = 2.0 * pi() * 2.54 * 1920.0 / (userTable.getCurrentUser()->cmp360 * userTable.getCurrentUser()->mouseDPI);
    // additional correction factor based on few samples - TODO: need more careful setup to study this
    mouseSensitivity = mouseSensitivity * 1.0675; // 10.5 / 10.0 * 30.5 / 30.0
    const shared_ptr<FirstPersonManipulator>& fpm = dynamic_pointer_cast<FirstPersonManipulator>(cameraManipulator());
    if (m_userSettingsMode) {
        // set to 3rd person
        fpm->setMouseMode(FirstPersonManipulator::MOUSE_DIRECT_RIGHT_BUTTON);
    }
    else {
        // Force into FPS mode
        fpm->setMouseMode(FirstPersonManipulator::MOUSE_DIRECT);
    }
	// Control player motion using the experiment config parameter
	fpm->setMoveRate(experimentConfig.moveRate);
    fpm->setTurnRate(mouseSensitivity);
}

/** Spawn a randomly parametrized target */
void App::spawnParameterizedRandomTarget(float motionDuration=4.0f, float motionDecisionPeriod=0.5f, float speed=2.0f, float radius=10.0f, float scale=2.0f) {
    Random& rng = Random::threadCommon();

    // Construct a reference frame
    // Remove the vertical component
    Vector3 Z = -activeCamera()->frame().lookVector();
    debugPrintf("lookatZ = [%.4f, %.4f, %.4f]\n", Z.x, Z.y, Z.z);
    debugPrintf("origin  = [%.4f, %.4f, %.4f]\n", activeCamera()->frame().translation.x, activeCamera()->frame().translation.y, activeCamera()->frame().translation.z);
    Z.y = 0.0f;
    Z = Z.direction();
    Vector3 Y = Vector3::unitY();
    Vector3 X = Y.cross(Z);

    // Make a random vector in front of the player in a narrow field of view
    Vector3 dir = (-Z + X * rng.uniform(-1, 1) + Y * rng.uniform(-0.5f, 0.5f)).direction();

    // Ray from user/camera toward intended spawn location
    Ray ray = Ray::fromOriginAndDirection(activeCamera()->frame().translation, dir);

    //distance = rng.uniform(2.0f, distance - 1.0f);
    const shared_ptr<FlyingEntity>& target =
        spawnTarget(ray.origin() + ray.direction() * radius,
            scale, false,
            Color3::wheelRandom());

    // Choose some destination locations based on speed and motionDuration
    const Point3& center = ray.origin();
    Array<Point3> destinationArray;
    // [radians/s] = [m/s] / [m/radians]
    float angularSpeed = speed / radius;
    // [rad] = [rad/s] * [s] 
    float angleChange = angularSpeed * motionDecisionPeriod;

    destinationArray.push(target->frame().translation);
    int tempInt = 0;
    for (float motionTime = 0.0f; motionTime < motionDuration; motionTime += motionDecisionPeriod) {
        // TODO: make angle change randomize correction, should be placed on circle around previous point
        float pitch = 0.0f;
        float yaw = tempInt++ % 2 == 0 ? angleChange : -angleChange;
        //float yaw = rng.uniform(-angleChange, angleChange);
        //float pitch = rng.uniform(-angleChange, angleChange);
        const Vector3& dir = CFrame::fromXYZYPRRadians(0.0f, 0.0f, 0.0f, yaw, pitch, 0.0f).rotation * ray.direction();
        ray.set(ray.origin(), dir);
        destinationArray.push(center + dir * radius);
    }
    target->setSpeed(speed); // m/s
    // debugging prints
    for (Point3* p = destinationArray.begin(); p != destinationArray.end(); ++p) {
        debugPrintf("[%.2f, %.2f, %.2f]\n", p->x, p->y, p->z);
    }
    target->setDestinations(destinationArray, center);
}

/** Spawn a random non-parametrized target */
void App::spawnRandomTarget() {
	Random& rng = Random::threadCommon();

	bool done = false;
	int tries = 0;

	// Construct a reference frame
	// Remove the vertical component
	Vector3 Z = -activeCamera()->frame().lookVector();
	Z.y = 0.0f;
	Z = Z.direction();
	Vector3 Y = Vector3::unitY();
	Vector3 X = Y.cross(Z);

	do {
		// Make a random vector in front of the player in a narrow field of view
		Vector3 dir = (-Z + X * rng.uniform(-1, 1) + Y * rng.uniform(-0.3f, 0.5f)).direction();

		// Make sure the spawn location is visible
		Ray ray = Ray::fromOriginAndDirection(activeCamera()->frame().translation, dir);
		float distance = finf();
		scene()->intersect(ray, distance);

		if ((distance > 2.0f) && (distance < finf())) {
            distance = rng.uniform(2.0f, distance - 1.0f);
			const shared_ptr<FlyingEntity>& target =
                spawnTarget(ray.origin() + ray.direction() * distance, 
                    rng.uniform(0.1f, 1.5f), rng.uniform() > 0.5f,
                    Color3::wheelRandom());

            // Choose some destination locations
            const Point3& center = ray.origin();
            Array<Point3> destinationArray;
            destinationArray.push(target->frame().translation);
            for (int i = 0; i < 20; ++i) {
        		const Vector3& dir = (-Z + X * rng.uniform(-1, 1) + Y * rng.uniform(-0.3f, 0.5f)).direction();
                destinationArray.push(center + dir * distance);
            }
            target->setSpeed(2.0f); // m/s
            target->setDestinations(destinationArray, center);

			done = true;
		}
		++tries;
	} while (!done && tries < 100);
}

/** Spawn a flying entity target */
shared_ptr<FlyingEntity> App::spawnTarget(const Point3& position, float scale, bool spinLeft, const Color3& color, String modelName) {
	const int scaleIndex = clamp(iRound(log(scale) / log(1.0f + TARGET_MODEL_ARRAY_SCALING) + TARGET_MODEL_ARRAY_OFFSET), 0, m_modelScaleCount - 1);

	const shared_ptr<FlyingEntity>& target = FlyingEntity::create(format("target%03d", ++m_lastUniqueID), scene().get(), m_targetModels[modelName][scaleIndex], CFrame());

	UniversalMaterial::Specification materialSpecification;
	materialSpecification.setLambertian(Texture::Specification(color));
	materialSpecification.setEmissive(Texture::Specification(color * 0.7f));
	materialSpecification.setGlossy(Texture::Specification(Color4(0.4f, 0.2f, 0.1f, 0.8f)));

	const shared_ptr<ArticulatedModel::Pose>& amPose = ArticulatedModel::Pose::create();
	amPose->materialTable.set("core/icosahedron_default", UniversalMaterial::create(materialSpecification));
	target->setPose(amPose);

	target->setFrame(position);
	/*
	// Don't set a track. We'll take care of the positioning after creation
	String animation = format("combine(orbit(0, %d), CFrame::fromXYZYPRDegrees(%f, %f, %f))", spinLeft ? 1 : -1, position.x, position.y, position.z);
	const shared_ptr<Entity::Track>& track = Entity::Track::create(target.get(), scene().get(), Any::parse(animation));
	target->setTrack(track);
	*/

	target->setShouldBeSaved(false);
	targetArray.append(target);
	scene()->insert(target);
	return target;
}

shared_ptr<FlyingEntity> App::spawnFlyingTarget(
	const Point3& position,
	float scale,
	const Color3& color,
	const Vector2& speedRange,
	const Vector2& motionChangePeriodRange,
	Point3 orbitCenter,
	String id,
	String name)
{
	const int scaleIndex = clamp(iRound(log(scale) / log(1.0f + TARGET_MODEL_ARRAY_SCALING) + TARGET_MODEL_ARRAY_OFFSET), 0, m_modelScaleCount - 1);
	String nameStr = name.empty() ? format("target%03d", ++m_lastUniqueID) : name;
	const shared_ptr<FlyingEntity>& target = FlyingEntity::create(
		nameStr,
		scene().get(),
		m_targetModels[id][scaleIndex],
		CFrame(),
		speedRange,
		motionChangePeriodRange,
		orbitCenter
	);

	UniversalMaterial::Specification materialSpecification;
	materialSpecification.setLambertian(Texture::Specification(color));
	materialSpecification.setEmissive(Texture::Specification(color * 0.7f));
	materialSpecification.setGlossy(Texture::Specification(Color4(0.4f, 0.2f, 0.1f, 0.8f)));

	const shared_ptr<ArticulatedModel::Pose>& amPose = ArticulatedModel::Pose::create();
	amPose->materialTable.set("core/icosahedron_default", UniversalMaterial::create(materialSpecification));
	target->setPose(amPose);

	target->setFrame(position);

	target->setShouldBeSaved(false);
	targetArray.append(target);
	scene()->insert(target);
	return target;
}

shared_ptr<JumpingEntity> App::spawnJumpingTarget(
	const Point3& position,
	float scale,
	const Color3& color,
    const Vector2& speedRange,
    const Vector2& motionChangePeriodRange,
    const Vector2& jumpPeriodRange,
	const Vector2& distanceRange,
	const Vector2& jumpSpeedRange,
	const Vector2& gravityRange,
	Point3 orbitCenter,
	float targetDistance,
	String id,
	String name)
{
	const int scaleIndex = clamp(iRound(log(scale) / log(1.0f + TARGET_MODEL_ARRAY_SCALING) + TARGET_MODEL_ARRAY_OFFSET), 0, m_modelScaleCount - 1);
	String nameStr = name.empty() ? format("target%03d", ++m_lastUniqueID) : name;
	const shared_ptr<JumpingEntity>& target = JumpingEntity::create(
		nameStr,
		scene().get(),
		m_targetModels[id][scaleIndex],
		CFrame(),
		speedRange,
		motionChangePeriodRange,
		jumpPeriodRange,
		distanceRange,
		jumpSpeedRange,
		gravityRange,
		orbitCenter,
		targetDistance
	);

	UniversalMaterial::Specification materialSpecification;
	materialSpecification.setLambertian(Texture::Specification(color));
	materialSpecification.setEmissive(Texture::Specification(color * 0.7f));
	materialSpecification.setGlossy(Texture::Specification(Color4(0.4f, 0.2f, 0.1f, 0.8f)));

	const shared_ptr<ArticulatedModel::Pose>& amPose = ArticulatedModel::Pose::create();
	amPose->materialTable.set("core/icosahedron_default", UniversalMaterial::create(materialSpecification));
	target->setPose(amPose);

	target->setFrame(position);

	target->setShouldBeSaved(false);
	targetArray.append(target);
	scene()->insert(target);
	return target;
}

void App::loadModels() {
	m_viewModel = ArticulatedModel::create(experimentConfig.weapon.modelSpec, "viewModel");

	const static Any bulletSpec = PARSE_ANY(ArticulatedModel::Specification{
		filename = "ifs/d10.ifs";
		preprocess = {
			transformGeometry(all(), Matrix4::pitchDegrees(90));
			transformGeometry(all(), Matrix4::scale(0.05,0.05,2));
			setMaterial(all(), UniversalMaterial::Specification {
				lambertian = Color3(0);
			emissive = Power3(5,4,0);
			});
		}; });

	m_bulletModel = ArticulatedModel::create(bulletSpec, "bulletModel");

	const static Any decalSpec = PARSE_ANY(ArticulatedModel::Specification{
		filename = "ifs/square.ifs";
		preprocess = {
			transformGeometry(all(), Matrix4::scale(0.1, 0.1, 0.1));
			setMaterial(all(), UniversalMaterial::Specification{
				lambertian = Texture::Specification {
					filename = "bullet-decal-256x256.png";
					encoding = Color3(1, 1, 1);
				};
			});
		}; });

	m_decalModel = ArticulatedModel::create(decalSpec, "decalModel");

	const static Any explosionSpec = PARSE_ANY(ArticulatedModel::Specification{
		filename = "ifs/square.ifs";
		preprocess = {
			transformGeometry(all(), Matrix4::scale(0.1, 0.1, 0.1));
			//scaleAndOffsetTexCoord0(all(), 0.0769, 0);
			setMaterial(all(), UniversalMaterial::Specification{
				lambertian = Texture::Specification {
					//filename = "explosion_01_strip13.png";
					filename = "explosion_01.png";
					encoding = Color3(1, 1, 1);
				};
			});
		}; });

	m_explosionModel = ArticulatedModel::create(explosionSpec, "explosionModel");

	// Add all the unqiue targets to this list
	Table<String, Any> toBuild;
	for (TargetConfig target : experimentConfig.targets) {
		toBuild.set(target.id, target.modelSpec);
	}
	// Append the basic model automatically (used for dummy targets for now)
	toBuild.set("dummy", PARSE_ANY(ArticulatedModel::Specification{
		filename = "model/target/target.obj";
		cleanGeometrySettings = ArticulatedModel::CleanGeometrySettings{
					allowVertexMerging = true;
					forceComputeNormals = false;
					forceComputeTangents = false;
					forceVertexMerging = true;
					maxEdgeLength = inf;
					maxNormalWeldAngleDegrees = 0;
					maxSmoothAngleDegrees = 0;
		};
		scale = 0.25;
	}));

	// Scale the models into the m_targetModel table
	for (String id : toBuild.getKeys()) {
		Array<shared_ptr<ArticulatedModel>> models;
		for (int i = 0; i <= m_modelScaleCount; ++i) {
			const float scale = pow(1.0f + TARGET_MODEL_ARRAY_SCALING, float(i) - TARGET_MODEL_ARRAY_OFFSET);
			Any spec = toBuild.get(id);
			spec.set("scale", scale);
			models.push(ArticulatedModel::create(spec));
		}
		m_targetModels.set(id, models);
	}
}

void App::makeGUI() {
	debugWindow->setVisible(!startupConfig.playMode);
	developerWindow->setVisible(!startupConfig.playMode);
	developerWindow->sceneEditorWindow->setVisible(!startupConfig.playMode);
	developerWindow->cameraControlWindow->setVisible(!startupConfig.playMode);
	developerWindow->videoRecordDialog->setEnabled(true);

	const float SLIDER_SPACING = 35;
	debugPane->beginRow(); {
		debugPane->addCheckBox("Hitscan", &m_hitScan);
		debugPane->addCheckBox("Show Bullets", &experimentConfig.weapon.renderBullets);
		debugPane->addCheckBox("Weapon", &experimentConfig.weapon.renderModel);
		debugPane->addCheckBox("HUD", &experimentConfig.showHUD);
		debugPane->addCheckBox("FPS", &m_renderFPS);
		debugPane->addCheckBox("Turbo", &emergencyTurbo);
		static int frames = 0;
		GuiControl* c = nullptr;

        debugPane->addButton("Spawn", this, &App::spawnRandomTarget);
		debugPane->setNewChildSize(230.0f, -1.0f, 70.0f);
		c = debugPane->addNumberBox("Framerate", Pointer<float>(
			[&]() { return 1.0f / float(realTimeTargetDuration()); },
			[&](float f) {
			// convert to seconds from fps
			f = 1.0f / f;
			const float current = (float)realTimeTargetDuration();
			if (abs(f - current) > 1e-5f) {
				// Only set when there is a change, otherwise the simulation's deltas are confused.
				setFrameDuration(f, GApp::REAL_TIME);
			}}), "Hz", GuiTheme::LOG_SLIDER, 30.0f, 5000.0f); c->moveBy(SLIDER_SPACING, 0);
			c = debugPane->addNumberBox("Input Lag", &frames, "f", GuiTheme::LINEAR_SLIDER, 0, 60); c->setEnabled(false); c->moveBy(SLIDER_SPACING, 0);
			c = debugPane->addNumberBox("Display Lag", &m_displayLagFrames, "f", GuiTheme::LINEAR_SLIDER, 0, 60); c->moveBy(SLIDER_SPACING, 0);
			debugPane->addNumberBox("Reticle", &m_reticleIndex, "", GuiTheme::LINEAR_SLIDER, 0, numReticles, 1)->moveBy(SLIDER_SPACING, 0);
			debugPane->addNumberBox("Brightness", &m_sceneBrightness, "x", GuiTheme::LOG_SLIDER, 0.01f, 2.0f)->moveBy(SLIDER_SPACING, 0);
	} debugPane->endRow();
	// Add new row w/ player move rate control
	debugPane->beginRow(); {
		debugPane->setNewChildSize(150.0f, -1.0f, 70.0f);
		GuiControl* c = nullptr;
		c = debugPane->addNumberBox("Move Rate", &(experimentConfig.moveRate), "m/s", GuiTheme::NO_SLIDER, 0.0f, 100.0f, 0.1f);
	} debugPane->endRow();


    // set up user settings window
    m_userSettingsWindow = GuiWindow::create("User Settings", nullptr, 
        Rect2D::xywh((float)window()->width() * 0.5f - 200.0f, (float)window()->height() * 0.5f - 100.0f, 400.0f, 200.0f));
    addWidget(m_userSettingsWindow);
    GuiPane* p = m_userSettingsWindow->pane();
    m_currentUserPane = p->addPane("Current User Settings");
    updateUserGUI();

    m_ddCurrentUser = userTable.getCurrentUserIndex();
    p = p->addPane("Experiment Settings");
    p->beginRow();
        m_userDropDown = p->addDropDownList("User", userTable.getIds(), &m_ddCurrentUser);
	    p->addButton("Select User", this, &App::updateUser);
    p->endRow();
    p->beginRow();
        m_sessDropDown = p->addDropDownList("Session", Array<String>({}), &m_ddCurrentSession);
        updateSessionDropDown();
	    p->addButton("Select Session", this, &App::updateSessionPress);
    p->endRow();
    p->addButton("Quit", this, &App::quitRequest);
    m_userSettingsWindow->setVisible(m_userSettingsMode);

	debugWindow->pack();
	debugWindow->setRect(Rect2D::xywh(0, 0, (float)window()->width(), debugWindow->rect().height()));
}

void App::userSaveButtonPress(void) {
	// Save the any file
	Any a = Any(userTable);
	a.save(startupConfig.userConfig());
	logPrintf("User table saved.\n");			// Print message to log
}	

void App::updateUser(void){
	// Update the user if needed
	if (m_lastSeenUser != m_ddCurrentUser) {
		// This creates a new results file...
		if(m_sessDropDown->numElements() > 0) updateSession(updateSessionDropDown()[0]);
		String id = getDropDownUserId();
		m_lastSeenUser = m_ddCurrentUser;

        userTable.currentUser = id;
        updateUserGUI();
	}
	// Get new session list for (new) user
	updateSessionDropDown();
}

void App::updateUserGUI() {
    m_currentUserPane->removeAllChildren();
    m_currentUserPane->addLabel(format("Current User: %s", userTable.currentUser));
    m_mouseDPILabel = m_currentUserPane->addLabel(format("Mouse DPI: %f", userTable.getCurrentUser()->mouseDPI));
    m_currentUserPane->addNumberBox("Mouse 360", &(userTable.getCurrentUser()->cmp360), "cm", GuiTheme::LINEAR_SLIDER, 0.2, 100.0, 0.2);
    m_currentUserPane->addButton("Save cm/360", this, &App::userSaveButtonPress);
}

Array<String> App::updateSessionDropDown(void) {
	// Create updated session list
    String userId = userTable.getCurrentUser()->id;
	shared_ptr<UserSessionStatus> userStatus = userStatusTable.getUserStatus(userId);
	// If we have a user that doesn't have specified sessions
	if (userStatus == nullptr) {
		// Create a new user session status w/ no progress and default order (from experimentconfig.Any)
		logPrintf("User %s not found. Creating a new user w/ default session ordering.\n", userId);
		UserSessionStatus newStatus = UserSessionStatus();
		newStatus.id = userId;
		newStatus.sessionOrder = experimentConfig.getSessIds();
		userStatusTable.userInfo.append(newStatus);
		userStatus = userStatusTable.getUserStatus(userId);
		userStatusTable.toAny().save("userstatus.Any");
	}
	Array<String> remainingSess = {};
	for (int i = 0; i < userStatus->sessionOrder.size(); i++) {
        // user hasn't completed this session
        if (!userStatus->completedSessions.contains(userStatus->sessionOrder[i])) {
            remainingSess.append(userStatus->sessionOrder[i]);
        }
    }
	m_sessDropDown->setList(remainingSess);

	// Print message to log
	logPrintf("Updated %s's session drop down to:\n", userId);
	for (String id : remainingSess) {
		logPrintf("\t%s\n", id);
	}

	return remainingSess;
}

String App::getDropDownSessId(void) {
	if (m_sessDropDown->numElements() == 0) return "";
	return m_sessDropDown->get(m_ddCurrentSession);
}

String App::getDropDownUserId(void) {
	return m_userDropDown->get(m_ddCurrentUser);
}

void App::markSessComplete(String sessId) {
	// Add the session id to completed session array
	userStatusTable.addCompletedSession(userTable.currentUser, sessId);
	// Save the file to any
	userStatusTable.toAny().save("userstatus.Any");
	logPrintf("Marked session: %s complete for user %s.\n", sessId, userTable.currentUser);
}

shared_ptr<UserConfig> App::getCurrUser(void) {
	return userTable.getUserById(getDropDownUserId());
}

void App::updateSessionPress(void) {
	updateSession(getDropDownSessId());
}

String GetLastErrorString() {
	DWORD error = GetLastError();
	if (error){
		LPVOID lpMsgBuf;
		DWORD bufLen = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&lpMsgBuf, 0, NULL);
		if (bufLen){
			LPCSTR lpMsgStr = (LPCSTR)lpMsgBuf;
			std::string result(lpMsgStr, lpMsgStr + bufLen);
			LocalFree(lpMsgBuf);
			return String(result);
		}
	}
	return String();
}

void App::updateSession(String id) {
	if (!id.empty()) {
		// Get the new session config
		shared_ptr<SessionConfig> sessConfig = experimentConfig.getSessionConfigById(id);
		// Print message to log
		logPrintf("User selected session: %s. Updating now...\n", id);
		// apply frame lag
		m_displayLagFrames = sessConfig->frameDelay;

		// Set a maximum *finite* frame rate
		float dt = 0;
		if (sessConfig->frameRate > 0) dt = 1.0f / sessConfig->frameRate;
		else dt = 1.0f / float(window()->settings().refreshRate);
		setFrameDuration(dt, GApp::REAL_TIME);

		// Update session drop-down selection
		m_sessDropDown->setSelectedValue(id);
	}

	// Make sure all targets are cleared
	clearTargets();

	// Initialize the experiment (session) and logger
	ex = Experiment::create(this);
	logger = Logger::create();

	// Load the experiment scene if we haven't already (target only)
	if (!m_sceneLoaded) {
		loadScene(experimentConfig.sceneName);
		m_sceneLoaded = true;
	}

	// Check for need to start latency logging and if so run the logger now
	SystemConfig sysConfig = SystemConfig::load();
	m_logName = "../results/" + id + "_" + userTable.currentUser + "_" + String(Logger::genFileTimestamp());
	if (sysConfig.hasLogger) {
		// Handle running logger if we need to (terminate then merge results)
		if (m_loggerRunning) {
			killPythonLogger();
			pythonMergeLogs(m_logName);
		}
		// Run a new logger if we need to
		runPythonLogger(m_logName, sysConfig.loggerComPort, sysConfig.hasSync, sysConfig.syncComPort);
	}

	// Don't create a results file for a user w/ no sessions left
	if (m_sessDropDown->numElements() == 0) {
		logPrintf("No sessions remaining for selected user.\n");
	}
	// Create the results file here (but how do we make sure user set up name?)
	else {
		logger->createResultsFile(m_logName + ".db", userTable.currentUser);
		logPrintf("Created results file: %s.db\n", m_logName.c_str());
	}

	// TODO: Remove the following by invoking a call back.
	ex->onInit();
}

void App::mergeCurrentLogToCurrentDB() {
	logger->closeResultsFile();
	if (m_loggerRunning) {
		killPythonLogger();
		pythonMergeLogs(m_logName);
	}
}

void App::runPythonLogger(String logName, String com, bool hasSync, String syncComPort = "") {
	// Variables for creating process/getting handle
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	// Come up w/ command string
	String cmd = "python ../scripts/\"event logger\"/software/event_logger.py " + com + " \"" + logName + "\"";
	if (hasSync) cmd += " " + syncComPort;

    logPrintf("Running python command: '%s'\n", cmd.c_str());

	LPSTR command = LPSTR(cmd.c_str());
	if (!CreateProcess(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		logPrintf("Failed to start logger: %s\n", GetLastErrorString());
	}
	// Update logger management variables
	m_loggerRunning = true;
	m_loggerHandle = pi.hProcess;
}

void App::killPythonLogger() {
	if (m_loggerRunning) TerminateProcess(m_loggerHandle, 0);
	m_loggerRunning = false;
}

void App::quitRequest() {
    setExitCode(0);
	mergeCurrentLogToCurrentDB();
    //killPythonLogger();
}

bool App::pythonMergeLogs(String basename) {
	String dbFile = basename + ".db";
	String eventFile = basename + "_event.csv";

	// If we can't find either the db output file or the csv input return false
	if (!FileSystem::exists(dbFile) || !FileSystem::exists(eventFile)) return false;

	// Variables for creating process/getting handle
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	String cmd = "python ../scripts/\"event logger\"/software/event_log_insert.py " + eventFile + " " + dbFile;	
	LPSTR command = LPSTR(cmd.c_str());
	if (!CreateProcess(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		logPrintf("Failed to merge results: %s\n", GetLastErrorString());
	}
	return true;
}

void App::onAfterLoadScene(const Any& any, const String& sceneName) {
	// Set the active camera to the player
	if (experimentConfig.walkMode) {
		setActiveCamera(m_scene->typedEntity<Camera>("camera"));
		// For now make the player invisible (prevent issues w/ seeing model from inside)
		m_scene->typedEntity<PlayerEntity>("player")->setVisible(false);
		m_scene->setGravity(experimentConfig.playerGravity);
	}
	else {
		setActiveCamera(m_debugCamera);
	}
	activeCamera()->setFieldOfView(experimentConfig.hFoV * units::degrees(), FOVDirection::HORIZONTAL);
}


void App::onAI() {
	GApp::onAI();
	// Add non-simulation game logic and AI code here
}


void App::onNetwork() {
	GApp::onNetwork();
	// Poll net messages here
}


void App::onGraphics3D(RenderDevice* rd, Array<shared_ptr<Surface> >& surface) {

    if (m_displayLagFrames > 0) {
		// Need one more frame in the queue than we have frames of delay, to hold the current frame
		if (m_ldrDelayBufferQueue.size() <= m_displayLagFrames) {
			// Allocate new textures
			for (int i = m_displayLagFrames - m_ldrDelayBufferQueue.size(); i >= 0; --i) {
				m_ldrDelayBufferQueue.push(Framebuffer::create(Texture::createEmpty(format("Delay buffer %d", m_ldrDelayBufferQueue.size()), rd->width(), rd->height(), ImageFormat::RGB8())));
			}
			debugAssert(m_ldrDelayBufferQueue.size() == m_displayLagFrames + 1);
		}

		// When the display lag changes, we must be sure to be within range
		m_currentDelayBufferIndex = min(m_displayLagFrames, m_currentDelayBufferIndex);

		rd->pushState(m_ldrDelayBufferQueue[m_currentDelayBufferIndex]);
	}

	scene()->lightingEnvironment().ambientOcclusionSettings.enabled = ! emergencyTurbo;
	activeCamera()->filmSettings().setAntialiasingEnabled(! emergencyTurbo);
	activeCamera()->filmSettings().setBloomStrength(emergencyTurbo ? 0.0f : 0.5f);

	GApp::onGraphics3D(rd, surface);

	if (m_displayLagFrames > 0) {
		// Display the delayed frame
		rd->popState();
		rd->push2D(); {
			// Advance the pointer to the next, which is also the oldest frame
			m_currentDelayBufferIndex = (m_currentDelayBufferIndex + 1) % (m_displayLagFrames + 1);
			Draw::rect2D(rd->viewport(), rd, Color3::white(), m_ldrDelayBufferQueue[m_currentDelayBufferIndex]->texture(0), Sampler::buffer());
		} rd->pop2D();
	}
}

Point2 App::getViewDirection()
{   // returns (azimuth, elevation), where azimuth is 0 deg when straightahead and + for right, - for left.
	Point3 view_cartesian = activeCamera()->frame().lookVector();
	float az = atan2(- view_cartesian.z, - view_cartesian.x) * 180 / pif();
	float el = atan2(view_cartesian.y, sqrtf(view_cartesian.x * view_cartesian.x + view_cartesian.z * view_cartesian.z)) * 180 / pif();
	return Point2(az, el);
}

void App::onSimulation(RealTime rdt, SimTime sdt, SimTime idt) {

	// TODO (or NOTTODO): The following can be cleared at the cost of one more level of inheritance.
	ex->onSimulation(rdt, sdt, idt);

	// These are all we need from GApp::onSimulation() for walk mode
	if (experimentConfig.walkMode) {
		m_widgetManager->onSimulation(rdt, sdt, idt);
		if (scene()) { scene()->onSimulation(sdt); }
		if (scene()) { scene()->onSimulation(sdt); }
	}
	else {
		GApp::onSimulation(rdt, sdt, idt);
	}

    // make sure mouse sensitivity is set right
    if (m_userSettingsMode) {
        updateMouseSensitivity();
    }

	const RealTime now = System::time();
	for (int p = 0; p < projectileArray.size(); ++p) {
		const Projectile& projectile = projectileArray[p];

		if (!m_hitScan) {
			// Check for collisions
		}

		if (projectile.endTime < now) {
			// Expire
			projectileArray.fastRemove(p);
			--p;
		}
		else {
			// Animate
			projectile.entity->setFrame(projectile.entity->frame() + projectile.entity->frame().lookVector() * experimentConfig.weapon.bulletSpeed);
		}
	}

	// explosion animation
	if (notNull(m_explosion) && m_explosionEndTime < now) {
		scene()->remove(m_explosion);
		m_explosion = nullptr;
	}
	else {
		// could update animation here...
	}

	// Move the player if in walk mode
	if (experimentConfig.walkMode) {
		const shared_ptr<PlayerEntity>& p = m_scene->typedEntity<PlayerEntity>("player");
		if (notNull(p)) {
			CFrame c = p->frame();
			float height = p->crouched() ? experimentConfig.crouchHeight : experimentConfig.playerHeight;
			c.translation += Vector3(0, height, 0);		// Set the player to the right height
			c.rotation = c.rotation * Matrix3::fromAxisAngle(Vector3::unitX(), p->headTilt());
			activeCamera()->setFrame(c);
		}
	}

	// Example GUI dynamic layout code.  Resize the debugWindow to fill
	// the screen horizontally.
	debugWindow->setRect(Rect2D::xywh(0, 0, (float)window()->width(), debugWindow->rect().height()));

	// Check for completed session
	if (ex->moveOn) {
		String nextSess = userStatusTable.getNextSession(userTable.currentUser);
		updateSession(nextSess);
	}
}

bool App::onEvent(const GEvent& event) {
    // Override 'q', 'z', 'c', and 'e' keys
    if ((event.type == GEventType::KEY_DOWN) && 
        (event.key.keysym.sym == 'e'
            || event.key.keysym.sym == 'z'
            || event.key.keysym.sym == 'c'
            || event.key.keysym.sym == 'q')) {
        return true;
    }

	// Handle super-class events
	if (GApp::onEvent(event)) { return true; }

    if ((event.type == GEventType::KEY_DOWN) && (event.key.keysym.sym == GKey::KP_MINUS)) {
        quitRequest();
        return true;
    }

	if(experimentConfig.walkMode){
		if ((event.type == GEventType::KEY_DOWN) && (event.key.keysym.sym == GKey::LCTRL)) {
			m_scene->typedEntity<PlayerEntity>("player")->setCrouched(true);
			return true;
		}
		if ((event.type == GEventType::KEY_UP) && (event.key.keysym.sym == GKey::LCTRL)) {
			m_scene->typedEntity<PlayerEntity>("player")->setCrouched(false);
			return true;
		}
	}

	// If you need to track individual UI events, manage them here.
	// Return true if you want to prevent other parts of the system
	// from observing this specific event.
	//
	// For example,
	// if ((event.type == GEventType::GUI_ACTION) && (event.gui.control == m_button)) { ... return true; }
	// if ((event.type == GEventType::KEY_DOWN) && (event.key.keysym.sym == GKey::TAB)) { ... return true; }
	// if ((event.type == GEventType::KEY_DOWN) && (event.key.keysym.sym == 'p')) { ... return true; }

    if ((event.type == GEventType::KEY_DOWN) && (event.key.keysym.sym == GKey::ESCAPE || event.key.keysym.sym == GKey::TAB)) {
        m_userSettingsMode = !m_userSettingsMode;
        m_userSettingsWindow->setVisible(m_userSettingsMode);
        if (m_userSettingsMode) {
            // set focus so buttons properly highlight
            m_widgetManager->setFocusedWidget(m_userSettingsWindow);
        }
        // switch to first or 3rd person mode
        updateMouseSensitivity();
        return true;
    }
	return false;
}


void App::onPostProcessHDR3DEffects(RenderDevice *rd) {
	GApp::onPostProcessHDR3DEffects(rd);

	rd->push2D(); {
		// TODO: Is this the right place to call it?
		ex->onGraphics2D(rd);

		// Draw target health bars
		if (experimentConfig.showTargetHealthBars) {
			for (auto const& target : targetArray) {
				target->drawHealthBar(rd, *activeCamera(), *m_framebuffer, 
					experimentConfig.targetHealthBarSize, 
					experimentConfig.targetHealthBarOffset,
					experimentConfig.targetHealthBarBorderSize,
					experimentConfig.targetHealthBarColors,
					experimentConfig.targetHealthBarBorderColor);
			}
		}

		// Draw the combat text
		if (experimentConfig.showCombatText) {
			Array<int> toRemove;
			for (int i = 0; i < m_combatTextList.size(); i++) {
				bool remove = !m_combatTextList[i]->draw(rd, *activeCamera(), *m_framebuffer);
				if (remove) m_combatTextList[i] = nullptr;		// Null pointers to remove
			}
			// Remove the expired elements here
			m_combatTextList.removeNulls();
		}

		// Paint both sides by the width of latency measuring box.
		Point2 latencyRect = experimentConfig.clickPhotonSize;
		//Draw::rect2D(
		//	Rect2D::xywh(
		//		(float)m_framebuffer->width() * 0.0f,
		//		(float)m_framebuffer->height() * 0.0f,
		//		(float)m_framebuffer->width() * latencyRect.x,
		//		(float)m_framebuffer->height()
		//	), rd, Color3::black()
		//);
		//Draw::rect2D(
		//	Rect2D::xywh(
		//		(float)m_framebuffer->width() * (1.0f - latencyRect.x),
		//		(float)m_framebuffer->height() * 0.0f,
		//		(float)m_framebuffer->width() * latencyRect.x,
		//		(float)m_framebuffer->height()
		//	), rd, Color3::black()
		//);

        // weapon ready status
        if (experimentConfig.renderWeaponStatus) {
            float boxLeft = (float)m_framebuffer->width() * 0.0f;
            if (experimentConfig.weaponStatusSide == "right") {
                // swap side
                boxLeft = (float)m_framebuffer->width() * (1.0f - latencyRect.x);
            }

			// Draw the "active" cooldown box
            Draw::rect2D(
                Rect2D::xywh(
                boxLeft,
                 (float)m_framebuffer->height() * (float)(ex->weaponCooldownPercent()),
                    (float)m_framebuffer->width() * latencyRect.x,
                    (float)m_framebuffer->height() * (float)(1.0 - ex->weaponCooldownPercent())
                ), rd, Color3::white() * 0.8f
            );
        }

		// Click to photon latency measuring corner box
		if (experimentConfig.renderClickPhoton) {
			float boxLeft = 0.0f;
            if (experimentConfig.clickPhotonSide == "right") {
                // swap side
				boxLeft = (float)m_framebuffer->width() * (1.0f - latencyRect.x);
            }
			// Draw the "active" box
			Color3 cornerColor = (m_buttonUp) ? experimentConfig.clickPhotonColors[0] : experimentConfig.clickPhotonColors[1];
			Draw::rect2D(
				Rect2D::xywh(
					boxLeft,
					(float)m_framebuffer->height() * (experimentConfig.clickPhotonVertPos - latencyRect.y / 2),
					(float)m_framebuffer->width() * latencyRect.x,
					(float)m_framebuffer->height() * latencyRect.y
				), rd, cornerColor
			);
		}

		// Draw the HUD elements
		if (experimentConfig.showHUD) {
			// Draw the player health bar
			if (experimentConfig.showPlayerHealthBar) {
				const float health = m_scene->typedEntity<PlayerEntity>("player")->health();
				const Point2 location = experimentConfig.playerHealthBarPos;
				const Point2 size = experimentConfig.playerHealthBarSize;
				const Point2 border = experimentConfig.playerHealthBarBorderSize;
				const Color4 borderColor = experimentConfig.playerHealthBarBorderColor;
				const Color4 color = experimentConfig.playerHealthBarColors[1] * (1.0f - health) + experimentConfig.playerHealthBarColors[0] * health;

				Draw::rect2D(Rect2D::xywh(location - border, size + border + border), rd, borderColor);
				Draw::rect2D(Rect2D::xywh(location, size*Point2(health, 1.0f)), rd, color);
			}
			// Draw the ammo indicator
			if (experimentConfig.showAmmo) {
				Point2 lowerRight = Point2(m_framebuffer->width(), m_framebuffer->height());
				hudFont->draw2D(rd,
					format("%d/%d", ex->remainingAmmo(), experimentConfig.weapon.maxAmmo),
					lowerRight - experimentConfig.ammoPosition,
					experimentConfig.ammoSize,
					experimentConfig.ammoColor,
					experimentConfig.ammoOutlineColor,
					GFont::XALIGN_RIGHT,
					GFont::YALIGN_BOTTOM
				);
			}
		}

	} rd->pop2D();

	if (experimentConfig.shader != "") {
		// This code could be run more efficiently at LDR after Film::exposeAndRender or even during the
		// latency queue copy
			
		// Copy the post-VFX HDR framebuffer
		static shared_ptr<Framebuffer> temp = Framebuffer::create(Texture::createEmpty("temp distortion source", 256, 256, m_framebuffer->texture(0)->format()));
		temp->resize(m_framebuffer->width(), m_framebuffer->height());
		m_framebuffer->blitTo(rd, temp, false, false, false, false, true);

		rd->push2D(m_framebuffer); {
			Args args;
			args.setUniform("sourceTexture", temp->texture(0), Sampler::video());
			args.setRect(rd->viewport());
			LAUNCH_SHADER(experimentConfig.shader, args);
		} rd->pop2D();
	}
}

/** Method for handling weapon fire */
shared_ptr<TargetEntity> App::fire(bool destroyImmediately) {
    BEGIN_PROFILER_EVENT("fire");
	Point3 aimPoint = activeCamera()->frame().translation + activeCamera()->frame().lookVector() * 1000.0f;
	bool destroyedTarget = false;
	static bool hitTarget = false;
	static RealTime lastTime;
	shared_ptr<TargetEntity> target = nullptr;

	if (m_hitScan) {
		const Ray& ray = activeCamera()->frame().lookRay();		// Use the camera lookray for hit detection
			
		// Check for closest hit
		float closest = finf();
		int closestIndex = -1;
		for (int t = 0; t < targetArray.size(); ++t) {
			if (targetArray[t]->intersect(ray, closest)) {
				closestIndex = t;
			}
		}

		// Hit logic
		if (closestIndex >= 0) {
			target = targetArray[closestIndex];			// Assign the target pointer here (not null indicates the hit)

			// Damage the target
			float damage;
			if (destroyImmediately) damage = target->health();
			else if (experimentConfig.weapon.firePeriod == 0.0f && hitTarget) {		// Check if we are in "laser" mode hit the target last time
				float dt = static_cast<float>(System::time() - lastTime);
				damage = experimentConfig.weapon.damagePerSecond * dt;
			}
			else {																// If we're not in "laser" mode then damage/shot is just damage/second * second/shot
				damage = experimentConfig.weapon.damagePerSecond * experimentConfig.weapon.firePeriod;
			}
			lastTime = System::time();
			hitTarget = true;

			// Check if we need to add combat text for this damage
			if (experimentConfig.showCombatText) {
				m_combatTextList.append(FloatingCombatText::create(
					format("%2.0f", 100*damage),
					m_combatFont,
					experimentConfig.combatTextSize,
					experimentConfig.combatTextColor,
					experimentConfig.combatTextOutline,
					experimentConfig.combatTextOffset,
					experimentConfig.combatTextVelocity,
					experimentConfig.combatTextFade,
					experimentConfig.combatTextTimeout));
				m_combatTextList.last()->setFrame(target->frame());
			}

			bool destroyed = target->doDamage(damage); // TODO: health point should be tracked by Target Entity class (not existing yet).
			if (destroyed) {
				// create explosion animation
				CFrame explosionFrame = targetArray[closestIndex]->frame();
				explosionFrame.rotation = activeCamera()->frame().rotation;
				const shared_ptr<VisibleEntity>& newExplosion = VisibleEntity::create("explosion", scene().get(), m_explosionModel, explosionFrame);
				scene()->insert(newExplosion);
				m_explosion = newExplosion;
				m_explosionEndTime = System::time() + 0.1f; // make explosion end in 0.5 seconds
				destroyTarget(closestIndex);
				destroyedTarget = true;
				destroyedTargets += 1;
			}
			else {
                BEGIN_PROFILER_EVENT("fire/changeColor");
                    BEGIN_PROFILER_EVENT("fire/clone");
				        shared_ptr<ArticulatedModel::Pose> pose = dynamic_pointer_cast<ArticulatedModel::Pose>(targetArray[closestIndex]->pose()->clone());
                    END_PROFILER_EVENT();
                    BEGIN_PROFILER_EVENT("fire/materialSet");
						shared_ptr<UniversalMaterial> mat = m_materials[min((int)(target->health()*m_MatTableSize), m_MatTableSize-1)];
				        pose->materialTable.set("core/icosahedron_default", mat);
                    END_PROFILER_EVENT();
                    BEGIN_PROFILER_EVENT("fire/setPose");
				        targetArray[closestIndex]->setPose(pose);
                    END_PROFILER_EVENT();
                END_PROFILER_EVENT();
			}
		}
		else hitTarget = false;
	}

	// Create the bullet
	if (experimentConfig.weapon.renderBullets) {
		// Create the bullet start frame from the weapon frame plus muzzle offset
		CFrame bulletStartFrame = m_weaponFrame;
		bulletStartFrame.translation += experimentConfig.weapon.muzzleOffset;

		// Angle the bullet start frame towards the aim point
		bulletStartFrame.lookAt(aimPoint);

		bulletStartFrame.translation += bulletStartFrame.lookVector() * 2.0f;
		const shared_ptr<VisibleEntity>& bullet = VisibleEntity::create(format("bullet%03d", ++m_lastUniqueID), scene().get(), m_bulletModel, bulletStartFrame);
		bullet->setShouldBeSaved(false);
		bullet->setCanCauseCollisions(false);
		bullet->setCastsShadows(false);

		/*
		const shared_ptr<Entity::Track>& track = Entity::Track::create(bullet.get(), scene().get(),
			Any::parse(format("%s", bulletStartFrame.toXYZYPRDegreesString().c_str())));
		bullet->setTrack(track);
		*/

		projectileArray.push(Projectile(bullet, System::time() + 1.0f));
		scene()->insert(bullet);
	}

	if (startupConfig.playMode) {
		if (destroyedTarget) {
			m_explosionSound->play(10.0f);
			//m_explosionSound->play(target->frame().translation, Vector3::zero(), 50.0f);
		}
		else if(experimentConfig.weapon.firePeriod > 0.0f || !experimentConfig.weapon.autoFire) {
			m_fireSound->play(0.5f);
			//m_fireSound->play(activeCamera()->frame().translation, activeCamera()->frame().lookVector() * 2.0f, 0.5f);
		}
	}

	if (experimentConfig.weapon.renderDecals && experimentConfig.weapon.firePeriod > 0.0f && !hitTarget) {
		// compute world intersection
		const Ray& ray = activeCamera()->frame().lookRay();
		float hitDist = finf();
		Array<shared_ptr<Entity>> dontHit = { m_explosion, m_lastDecal, m_firstDecal };
		for (auto projectile : projectileArray) {
			dontHit.append(projectile.entity);
		}
		for (auto target : targetArray) {
			dontHit.append(target);
		}

		// Cast a ray against the scene to get the decal location/normal
		Model::HitInfo info;
		scene()->intersect(ray, hitDist, false, dontHit, info);
		// Find where to put the decal
		CFrame decalFrame = activeCamera()->frame();
		decalFrame.translation += ray.direction() * (hitDist - 0.01f);
		// Set the decal rotation to match the normal here
		decalFrame.lookAt(decalFrame.translation - info.normal);

		// Only allow 1 miss decal at a time (remove last decal if present)
		if (notNull(m_lastDecal)) {
			scene()->remove(m_lastDecal);
		}

		// Add the new decal to the scene
		const shared_ptr<VisibleEntity>& newDecal = VisibleEntity::create(format("decal%03d", ++m_lastUniqueID), scene().get(), m_decalModel, decalFrame);
		newDecal->setCastsShadows(false);
		scene()->insert(newDecal);
		m_lastDecal = m_firstDecal;
		m_firstDecal = newDecal;
	}
    END_PROFILER_EVENT();
	return target;
}

/** Clear all targets one by one */
void App::clearTargets() {
	while (targetArray.size() > 0) {
		destroyTarget(0);
	}
}

/** Handle user input here */
void App::onUserInput(UserInput* ui) {
    BEGIN_PROFILER_EVENT("onUserInput");
	static bool haveReleased = false;
	static bool fired = false;
	GApp::onUserInput(ui);
	(void)ui;

	if (experimentConfig.walkMode && !m_userSettingsMode) {
		const shared_ptr<PlayerEntity>& player = m_scene->typedEntity<PlayerEntity>("player");
		if (notNull(player)) {
			const float walkSpeed = experimentConfig.moveRate * units::meters() / units::seconds();
			const float pixelsPerRevolution = 30;
			const float   turnRatePerPixel = -pixelsPerRevolution * units::degrees() / (units::seconds());
			const float   tiltRatePerPixel = -0.2f * units::degrees() / (units::seconds());
			static const Vector3 jumpVelocity(0, experimentConfig.jumpVelocity * units::meters() / units::seconds(), 0);

			// Get walking speed here (and normalize if necessary)
			Vector3 linear = Vector3(ui->getX(), 0, -ui->getY());
			if (linear.magnitude() > 0) {
				linear = linear.direction() * walkSpeed;
			}
			// Add jump here (if needed)
			if (ui->keyPressed(GKey::SPACE)) {
				linear += jumpVelocity;
			}
			else {
				linear += Vector3(0, player->desiredOSVelocity().y, 0);
			}

			// Get the mouse rotation here
			float yaw = ui->mouseDX() * turnRatePerPixel;
			float pitch = ui->mouseDY() * tiltRatePerPixel;

			// Set the player translation/view velocities
			player->setDesiredOSVelocity(linear);
			player->setDesiredAngularVelocity(yaw, pitch);
		}
	}

	// Require release between clicks for non-autoFire modes
	if (ui->keyReleased(GKey::LEFT_MOUSE)) {
		m_buttonUp = true;
		if (!experimentConfig.weapon.autoFire) {
			haveReleased = true;
			fired = false;
		}
	}

	// Handle the mouse down events
	if (ui->keyDown(GKey::LEFT_MOUSE)) {
		if (experimentConfig.weapon.autoFire || haveReleased) {		// Make sure we are either in autoFire mode or have seen a release of the mouse
			// check for hit, add graphics, update target state
			if (ex->presentationState == PresentationState::task) {
				if (ex->responseReady()) {
					fired = true;
					ex->countClick();						        // Count clicks
					shared_ptr<TargetEntity> t = fire();			// Fire the weapon
					if (notNull(t)) {								// Check if we hit anything
                        if (t->health() <= 0) {
                            // Target eliminated, must be 'destroy'.
                            ex->accumulatePlayerAction("destroy", t->name());	
                        }
                        else {
                            // Target 'hit', but still alive.
                            ex->accumulatePlayerAction("hit", t->name());
                        }
					}
                    else {
                        // Target still present, must be 'miss'.
                        ex->accumulatePlayerAction("miss");
                    }
				}
				// Avoid accumulating invalid clicks during holds...
                else {
                    // Invalid click since the trial isn't ready for response
                    ex->accumulatePlayerAction("invalid");
                }
			}
		}
		else {
			ex->accumulatePlayerAction("non-task"); // not happening in task state.
		}
		haveReleased = false;					// Make it known we are no longer in released state
		m_buttonUp = false;
	}
	
	// Handle spacebar during feedback
	GKey initShootKey = experimentConfig.walkMode ? GKey::LSHIFT : GKey::SPACE;
	if (ui->keyPressed(initShootKey) && (ex->presentationState == PresentationState::feedback)) {
		fire(true); // Space for ready target (destroy this immediately regardless of weapon)
	}

	if(ui->keyDown(GKey::LCTRL))

	if (m_lastReticleLoaded != m_reticleIndex) {
		// Slider was used to change the reticle
		setReticle(m_reticleIndex);
	}

	activeCamera()->filmSettings().setSensitivity(m_sceneBrightness);
    END_PROFILER_EVENT();
}

void App::destroyTarget(int index) {
	// Not a reference because we're about to manipulate the array
	const shared_ptr<VisibleEntity> target = targetArray[index];
	// Remove the target from the target array
	targetArray.fastRemove(index);
	// Remove the target from the scene
	scene()->removeEntity(target->name());
}

void App::onPose(Array<shared_ptr<Surface> >& surface, Array<shared_ptr<Surface2D> >& surface2D) {
	GApp::onPose(surface, surface2D);

	if (experimentConfig.walkMode) {
		m_scene->poseExceptExcluded(surface, "player");
	}

	if (experimentConfig.weapon.renderModel) {
		const float yScale = -0.12f;
		const float zScale = -yScale * 0.5f;
		const float lookY = activeCamera()->frame().lookVector().y;
		const float prevLookY = activeCamera()->previousFrame().lookVector().y;
		m_weaponFrame = activeCamera()->frame() * CFrame::fromXYZYPRDegrees(0.3f, -0.4f + lookY * yScale, -1.1f + lookY * zScale, 10, 5);
		const CFrame prevWeaponPos = CFrame::fromXYZYPRDegrees(0.3f, -0.4f + prevLookY * yScale, -1.1f + prevLookY * zScale, 10, 5);
		m_viewModel->pose(surface, m_weaponFrame, activeCamera()->previousFrame() * prevWeaponPos, nullptr, nullptr, nullptr, Surface::ExpressiveLightScatteringProperties());
	}
}

void App::onGraphics2D(RenderDevice* rd, Array<shared_ptr<Surface2D>>& posed2D) {
 
 //   // Faster than the full stats widget
	//std::string expDebugStr = "%d fps ";
	//expDebugStr += ex->getDebugStr(); // debugging message
 //   debugFont->draw2D(rd, format(expDebugStr.c_str(), iRound(renderDevice->stats().smoothFrameRate)), Point2(10,10), 12.0f, Color3::yellow());

    // Render 2D objects like Widgets.  These do not receive tone mapping or gamma correction.

	// Track the instantaneous frame duration (no smoothing) in a circular queue
	if (m_frameDurationQueue.length() > MAX_HISTORY_TIMING_FRAMES) {
		m_frameDurationQueue.dequeue();
	}
	{
		const float f = rd->stats().frameRate;
		const float t = 1.0f / f;
		m_frameDurationQueue.enqueue(t);
	}

	float recentMin = finf();
	float recentMax = -finf();
	for (int i = 0; i < m_frameDurationQueue.length(); ++i) {
		const float t = m_frameDurationQueue[i];
		recentMin = min(recentMin, t);
		recentMax = max(recentMax, t);
	}

	rd->push2D(); {
		const float scale = rd->viewport().width() / 1920.0f;

		// FPS display (faster than the full stats widget)
		if (m_renderFPS) {
			String msg;

			if (window()->settings().refreshRate > 0) {
				msg = format("%d measured / %d requested fps",
					iRound(renderDevice->stats().smoothFrameRate),
					window()->settings().refreshRate);
			}
			else {
				msg = format("%d fps", iRound(renderDevice->stats().smoothFrameRate));
			}

			msg += format(" | %.1f min/%.1f avg/%.1f max ms", recentMin * 1000.0f, 1000.0f / renderDevice->stats().smoothFrameRate, 1000.0f * recentMax);

			outputFont->draw2D(rd, msg, (Point2(30, 28) * scale).floor(), floor(20.0f * scale), Color3::yellow());
		}

	} rd->pop2D();

	// Might not need this on the reaction trial
	// This is rendering the GUI. Can remove if desired.
	Surface2D::sortAndRender(rd, posed2D);
}

/** Set the currently reticle by index */
void App::setReticle(int r) {
	m_lastReticleLoaded = m_reticleIndex = clamp(0, r, numReticles);
	if (r < numReticles) {
		reticleTexture = Texture::fromFile(System::findDataFile(format("gui/reticle/reticle-%03d.png", m_reticleIndex)));
	}
	else {
		// This special case is added to allow a custom reticle not in the gui/reticle/reticle-[x].png format
		reticleTexture = Texture::fromFile(System::findDataFile("gui/reticle.png"));
	}
}

void App::onCleanup() {
	// Called after the application loop ends.  Place a majority of cleanup code
	// here instead of in the constructor so that exceptions can be caught.
}

/** Overridden (optimized) oneFrame() function to improve latency */
void App::oneFrame() {

    // Wait
    // Note: we might end up spending all of our time inside of
    // RenderDevice::beginFrame.  Waiting here isn't double waiting,
    // though, because while we're sleeping the CPU the GPU is working
    // to catch up.    
    if ((submitToDisplayMode() == SubmitToDisplayMode::MINIMIZE_LATENCY)) {
        BEGIN_PROFILER_EVENT("Wait");
        m_waitWatch.tick(); {
            RealTime nowAfterLoop = System::time();

            // Compute accumulated time
            RealTime cumulativeTime = nowAfterLoop - m_lastWaitTime;

            debugAssert(m_wallClockTargetDuration < finf());
            // Perform wait for actual time needed
            RealTime duration = m_wallClockTargetDuration;
            if (!window()->hasFocus() && m_lowerFrameRateInBackground) {
                // Lower frame rate to 4fps
                duration = 1.0 / 4.0;
            }
            RealTime desiredWaitTime = max(0.0, duration - cumulativeTime);
            onWait(max(0.0, desiredWaitTime - m_lastFrameOverWait) * 0.97);

            // Update wait timers
            m_lastWaitTime = System::time();
            RealTime actualWaitTime = m_lastWaitTime - nowAfterLoop;

            // Learn how much onWait appears to overshoot by and compensate
            double thisOverWait = actualWaitTime - desiredWaitTime;
            if (G3D::abs(thisOverWait - m_lastFrameOverWait) / max(G3D::abs(m_lastFrameOverWait), G3D::abs(thisOverWait)) > 0.4) {
                // Abruptly change our estimate
                m_lastFrameOverWait = thisOverWait;
            }
            else {
                // Smoothly change our estimate
                m_lastFrameOverWait = lerp(m_lastFrameOverWait, thisOverWait, 0.1);
            }
        }  m_waitWatch.tock();
        END_PROFILER_EVENT();
    }

    for (int repeat = 0; repeat < max(1, m_renderPeriod); ++repeat) {
        Profiler::nextFrame();
        m_lastTime = m_now;
        m_now = System::time();
        RealTime timeStep = m_now - m_lastTime;

        // User input
        m_userInputWatch.tick();
        if (manageUserInput) {
            processGEventQueue();
        }
        onAfterEvents();
        onUserInput(userInput);
        m_userInputWatch.tock();

        // Network
        BEGIN_PROFILER_EVENT("GApp::onNetwork");
        m_networkWatch.tick();
        onNetwork();
        m_networkWatch.tock();
        END_PROFILER_EVENT();

        // Logic
        m_logicWatch.tick();
        {
            onAI();
        }
        m_logicWatch.tock();

        // Simulation
        m_simulationWatch.tick();
        BEGIN_PROFILER_EVENT("Simulation");
        {
            RealTime rdt = timeStep;

            SimTime sdt = m_simTimeStep;
            if (sdt == MATCH_REAL_TIME_TARGET) {
                sdt = m_wallClockTargetDuration;
            }
            else if (sdt == REAL_TIME) {
                sdt = float(timeStep);
            }
            sdt *= m_simTimeScale;

            SimTime idt = m_wallClockTargetDuration;

            onBeforeSimulation(rdt, sdt, idt);
            onSimulation(rdt, sdt, idt);
            onAfterSimulation(rdt, sdt, idt);

            m_previousSimTimeStep = float(sdt);
            m_previousRealTimeStep = float(rdt);
            setRealTime(realTime() + rdt);
            setSimTime(simTime() + sdt);
        }
        m_simulationWatch.tock();
        END_PROFILER_EVENT();
    }


    // Pose
    BEGIN_PROFILER_EVENT("Pose");
    m_poseWatch.tick(); {
        m_posed3D.fastClear();
        m_posed2D.fastClear();
        onPose(m_posed3D, m_posed2D);

        // The debug camera is not in the scene, so we have
        // to explicitly pose it. This actually does nothing, but
        // it allows us to trigger the TAA code.
		activeCamera()->onPose(m_posed3D);
    } m_poseWatch.tock();
    END_PROFILER_EVENT();

    // Wait
    // Note: we might end up spending all of our time inside of
    // RenderDevice::beginFrame.  Waiting here isn't double waiting,
    // though, because while we're sleeping the CPU the GPU is working
    // to catch up.    
    if ((submitToDisplayMode() != SubmitToDisplayMode::MINIMIZE_LATENCY)) {
        BEGIN_PROFILER_EVENT("Wait");
        m_waitWatch.tick(); {
            RealTime nowAfterLoop = System::time();

            // Compute accumulated time
            RealTime cumulativeTime = nowAfterLoop - m_lastWaitTime;

            debugAssert(m_wallClockTargetDuration < finf());
            // Perform wait for actual time needed
            RealTime duration = m_wallClockTargetDuration;
            if (!window()->hasFocus() && m_lowerFrameRateInBackground) {
                // Lower frame rate to 4fps
                duration = 1.0 / 4.0;
            }
            RealTime desiredWaitTime = max(0.0, duration - cumulativeTime);
            onWait(max(0.0, desiredWaitTime - m_lastFrameOverWait) * 0.97);

            // Update wait timers
            m_lastWaitTime = System::time();
            RealTime actualWaitTime = m_lastWaitTime - nowAfterLoop;

            // Learn how much onWait appears to overshoot by and compensate
            double thisOverWait = actualWaitTime - desiredWaitTime;
            if (G3D::abs(thisOverWait - m_lastFrameOverWait) / max(G3D::abs(m_lastFrameOverWait), G3D::abs(thisOverWait)) > 0.4) {
                // Abruptly change our estimate
                m_lastFrameOverWait = thisOverWait;
            }
            else {
                // Smoothly change our estimate
                m_lastFrameOverWait = lerp(m_lastFrameOverWait, thisOverWait, 0.1);
            }
        }  m_waitWatch.tock();
        END_PROFILER_EVENT();
    }

    // Graphics
    debugAssertGLOk();
    if ((submitToDisplayMode() == SubmitToDisplayMode::BALANCE) && (!renderDevice->swapBuffersAutomatically())) {
        swapBuffers();
    }

    if (notNull(m_gazeTracker)) {
        BEGIN_PROFILER_EVENT("Gaze Tracker");
        sampleGazeTrackerData();
        END_PROFILER_EVENT();
    }

    BEGIN_PROFILER_EVENT("Graphics");
    renderDevice->beginFrame();
    m_widgetManager->onBeforeGraphics();
    m_graphicsWatch.tick(); {
        debugAssertGLOk();
        renderDevice->pushState(); {
            debugAssertGLOk();
            onGraphics(renderDevice, m_posed3D, m_posed2D);
        } renderDevice->popState();
    }  m_graphicsWatch.tock();
    renderDevice->endFrame();
    if ((submitToDisplayMode() == SubmitToDisplayMode::MINIMIZE_LATENCY) && (!renderDevice->swapBuffersAutomatically())) {
        swapBuffers();
    }
    END_PROFILER_EVENT();

    // Remove all expired debug shapes
    for (int i = 0; i < debugShapeArray.size(); ++i) {
        if (debugShapeArray[i].endTime <= m_now) {
            debugShapeArray.fastRemove(i);
            --i;
        }
    }

    for (int i = 0; i < debugLabelArray.size(); ++i) {
        if (debugLabelArray[i].endTime <= m_now) {
            debugLabelArray.fastRemove(i);
            --i;
        }
    }

    debugText.fastClear();

    m_posed3D.fastClear();
    m_posed2D.fastClear();

    if (m_endProgram && window()->requiresMainLoop()) {
        window()->popLoopBody();
    }
}


// Tells C++ to invoke command-line main() function even on OS X and Win32.
G3D_START_AT_MAIN();

int main(int argc, const char* argv[]) {

    if (FileSystem::exists("startupconfig.Any")) {
        startupConfig = Any::fromFile("startupconfig.Any");
    }
    else {
        // autogenerate if it wasn't there
        startupConfig.toAny().save("startupconfig.Any");
    }

	{
		G3DSpecification spec;
		spec.audio = startupConfig.playMode;
		initGLG3D(spec);
	}

	(void)argc; (void)argv;
	GApp::Settings settings(argc, argv);

	if (startupConfig.playMode) {
		settings.window.width = 1920; settings.window.height = 1080;
	}
	else {
		settings.window.width = 1920; settings.window.height = 980;
	}
	settings.window.fullScreen = startupConfig.playMode;
	settings.window.resizable = !settings.window.fullScreen;

    // V-sync off always
	settings.window.asynchronous = true;
	settings.window.caption = "NVIDIA Abstract FPS";
	settings.window.refreshRate = -1;
	settings.window.defaultIconFilename = "icon.png";

	settings.hdrFramebuffer.depthGuardBandThickness = Vector2int16(64, 64);
	settings.hdrFramebuffer.colorGuardBandThickness = Vector2int16(0, 0);
	settings.dataDir = FileSystem::currentDirectory();
	settings.screenCapture.includeAppRevision = false;
	settings.screenCapture.includeG3DRevision = false;
	settings.screenCapture.outputDirectory = ""; // "../journal/"
	settings.screenCapture.filenamePrefix = "_";

	settings.renderer.deferredShading = true;
	settings.renderer.orderIndependentTransparency = false;

	return App(settings).run();
}

