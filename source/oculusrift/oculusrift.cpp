/*

	Might be important:
		https://forums.oculus.com/viewtopic.php?f=20&t=24361&p=283304&hilit=opengl+0.6#p283304
	Might be useful:
		https://forums.oculus.com/viewtopic.php?f=39&t=91&p=277330&hilit=opengl+0.6#p277330


*/

// a bunch of likely Max includes:
extern "C" {
#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_systhread.h"
	
#include "z_dsp.h"
	
#include "jit.common.h"
#include "jit.gl.h"
}

#include <new> // for in-place constructor

#include "OVR_CAPI.h"
#include "OVR_CAPI_GL.h"

#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8                   0x8C43
#endif

static t_class * max_class = 0;

static t_symbol * ps_quat;
static t_symbol * ps_pos;
static t_symbol * ps_viewport;
static t_symbol * ps_frustum;
static t_symbol * ps_warning;
static t_symbol * ps_glid;
static t_symbol * ps_jit_gl_texture;

// TODO: this is a really annoying hack. The Oculus driver doesn't seem to like being reconnected too quickly 
// -- it says it reconnects, but the display remains blank or noisy.
// inserting a short wait seems to avoid it. This wait in terms of frames:
#define RECONNECTION_TIME 100

class oculusrift {
public:
	t_object ob; // must be first!
	void * ob3d;
	void * outlet_msg;
	void * outlet_tracking;
	void * outlet_node;
	void * outlet_eye[2];
	void * outlet_tex;

	t_symbol * intexture;
	float near_clip, far_clip;
	float pixel_density;
	int max_fov;
	int perfMode;

	int reconnect_wait;

	ovrSession session;
	ovrGraphicsLuid luid;
	ovrEyeRenderDesc eyeRenderDesc[2];
	ovrVector3f      hmdToEyeViewOffset[2];
	ovrLayerEyeFov layer;
	ovrSizei pTextureDim;
	ovrSwapTextureSet * pTextureSet;
	ovrTexture * mirrorTexture;
	long long frameIndex;

	GLuint rbo;
	GLuint fbo1;

	oculusrift(t_symbol * dest_name) {

		// init Max object:
		jit_ob3d_new(this, dest_name);
		// outlets create in reverse order:
		outlet_msg = outlet_new(&ob, NULL);
		outlet_tracking = outlet_new(&ob, NULL);
		outlet_node = outlet_new(&ob, NULL);
		outlet_eye[1] = outlet_new(&ob, NULL);
		outlet_eye[0] = outlet_new(&ob, NULL);
		outlet_tex = outlet_new(&ob, "jit_gl_texture");

		// init state
		fbo1 = 0;
		rbo = 0;
		pTextureSet = 0;
		frameIndex = 0;
		pTextureDim.w = 0;
		pTextureDim.h = 0;

		// init attrs
		perfMode = 0;
		near_clip = 0.15f;
		far_clip = 100.f;
		pixel_density = 1.f;
		max_fov = 0;

		reconnect_wait = 0;

	}

	// attempt to connect to the OVR runtime, creating a session:
	bool connect() {
		post("connect");
		if (session) {
			object_warn(&ob, "already connected");
			return true;
		}

		ovrResult result;

		result = ovr_Create(&session, &luid);
		if (OVR_FAILURE(result)) {
			ovrErrorInfo errInfo;
			ovr_GetLastErrorInfo(&errInfo);
			object_error(&ob, "failed to create session: %s", errInfo.ErrorString);

			object_error(NULL, errInfo.ErrorString);
			return false;
		}

		object_post(&ob, "LibOVR runtime version %s", ovr_GetVersionString());

		outlet_anything(outlet_msg, gensym("connected"), 0, NULL);

		configure();
		return true;
	}

	void disconnect() {
		post("disconnect");
		if (session) {
			// destroy any OVR resources tied to the session:
			textureset_destroy();
			mirror_destroy();

			ovr_Destroy(session);
			session = 0;
			
			outlet_anything(outlet_msg, gensym("disconnected"), 0, NULL);


			reconnect_wait = RECONNECTION_TIME;
		}

		
	}

	// usually called after session is created, and when important attributes are changed
	// invokes info() to send configuration results 
	void configure() {
		post("configure");
		if (!session) {
			object_error(&ob, "no session to configure");
			return;
		}

		// maybe never: support disabling tracking options via ovr_ConfigureTracking()

		ovrHmdDesc hmd = ovr_GetHmdDesc(session);
		// Use hmd members and ovr_GetFovTextureSize() to determine graphics configuration

		ovrSizei recommenedTex0Size, recommenedTex1Size;
		//MaxEyeFov - Maximum optical field of view that can be practically rendered for each eye.
		if (max_fov){
			recommenedTex0Size = ovr_GetFovTextureSize(session, ovrEye_Left, hmd.MaxEyeFov[0], pixel_density);
			recommenedTex1Size = ovr_GetFovTextureSize(session, ovrEye_Right, hmd.MaxEyeFov[1], pixel_density);
		}
		else{
			recommenedTex0Size = ovr_GetFovTextureSize(session, ovrEye_Left, hmd.DefaultEyeFov[0], pixel_density);
			recommenedTex1Size = ovr_GetFovTextureSize(session, ovrEye_Right, hmd.DefaultEyeFov[1], pixel_density);
		}


		// assumes a single shared texture for both eyes:
		pTextureDim.w = recommenedTex0Size.w + recommenedTex1Size.w;
		pTextureDim.h = max(recommenedTex0Size.h, recommenedTex1Size.h);

		// Initialize VR structures, filling out description.
		if (max_fov) {
			eyeRenderDesc[0] = ovr_GetRenderDesc(session, ovrEye_Left, hmd.MaxEyeFov[0]);
			eyeRenderDesc[1] = ovr_GetRenderDesc(session, ovrEye_Right, hmd.MaxEyeFov[1]);

		}
		else {
			eyeRenderDesc[0] = ovr_GetRenderDesc(session, ovrEye_Left, hmd.DefaultEyeFov[0]);
			eyeRenderDesc[1] = ovr_GetRenderDesc(session, ovrEye_Right, hmd.DefaultEyeFov[1]);
		}
		hmdToEyeViewOffset[0] = eyeRenderDesc[0].HmdToEyeViewOffset;
		hmdToEyeViewOffset[1] = eyeRenderDesc[1].HmdToEyeViewOffset;

		// in case this is a re-configure, clear out the previous ones:
		textureset_destroy();
		mirror_destroy();

		textureset_create();
		mirror_create();

		// Initialize our single full screen Fov layer.
		// (needs to happen after textureset_create)
		layer.Header.Type = ovrLayerType_EyeFov;
		layer.Header.Flags = 0;
		layer.ColorTexture[0] = pTextureSet;
		layer.ColorTexture[1] = pTextureSet;
		layer.Fov[0] = eyeRenderDesc[0].Fov;
		layer.Fov[1] = eyeRenderDesc[1].Fov;
		layer.Viewport[0].Pos.x = 0;
		layer.Viewport[0].Pos.y = 0;
		layer.Viewport[0].Size.w = pTextureDim.w / 2;
		layer.Viewport[0].Size.h = pTextureDim.h;
		layer.Viewport[1].Pos.x = pTextureDim.w / 2;
		layer.Viewport[1].Pos.y = 0;
		layer.Viewport[1].Size.w = pTextureDim.w / 2;
		layer.Viewport[1].Size.h = pTextureDim.h;

		// ld.RenderPose and ld.SensorSampleTime are updated later per frame.
		
		info();
	}

	void info() {
		post("info");
		if (!session) {
			object_warn(&ob, "no session");
			return;
		}

		ovrHmdDesc hmd = ovr_GetHmdDesc(session);
		t_atom a[2];

		// TODO complete list of useful info from https://developer.oculus.com/documentation/pcsdk/latest/concepts/dg-sensor/
#define HMD_CASE(T) case T: { \
            atom_setsym(a, gensym( #T )); \
            outlet_anything(outlet_msg, gensym("hmdType"), 1, a); \
            break; \
			        }
		switch (hmd.Type) {
			HMD_CASE(ovrHmd_DK1)
				HMD_CASE(ovrHmd_DKHD)
				HMD_CASE(ovrHmd_DK2)
		default: {
				atom_setsym(a, gensym("unknown"));
				outlet_anything(outlet_msg, gensym("Type"), 1, a);
			}
		}
#undef HMD_CASE

		atom_setsym(a, gensym(hmd.SerialNumber));
		outlet_anything(outlet_msg, gensym("serial"), 1, a);

		atom_setsym(a, gensym(hmd.Manufacturer));
		outlet_anything(outlet_msg, gensym("Manufacturer"), 1, a);
		atom_setsym(a, gensym(hmd.ProductName));
		outlet_anything(outlet_msg, gensym("ProductName"), 1, a);

		atom_setlong(a, (hmd.VendorId));
		outlet_anything(outlet_msg, gensym("VendorId"), 1, a);
		atom_setlong(a, (hmd.ProductId));
		outlet_anything(outlet_msg, gensym("ProductId"), 1, a);
		atom_setfloat(a, (hmd.CameraFrustumHFovInRadians));
		outlet_anything(outlet_msg, gensym("CameraFrustumHFovInRadians"), 1, a);
		atom_setfloat(a, (hmd.CameraFrustumVFovInRadians));
		outlet_anything(outlet_msg, gensym("CameraFrustumVFovInRadians"), 1, a);
		atom_setfloat(a, (hmd.CameraFrustumNearZInMeters));
		outlet_anything(outlet_msg, gensym("CameraFrustumNearZInMeters"), 1, a);
		atom_setfloat(a, (hmd.CameraFrustumFarZInMeters));
		outlet_anything(outlet_msg, gensym("CameraFrustumFarZInMeters"), 1, a);
		atom_setlong(a, (hmd.AvailableHmdCaps));
		outlet_anything(outlet_msg, gensym("AvailableHmdCaps"), 1, a);
		atom_setlong(a, (hmd.DefaultHmdCaps));
		outlet_anything(outlet_msg, gensym("DefaultHmdCaps"), 1, a);
		atom_setlong(a, (hmd.AvailableTrackingCaps));
		outlet_anything(outlet_msg, gensym("AvailableTrackingCaps"), 1, a);
		atom_setlong(a, (hmd.DefaultTrackingCaps));
		outlet_anything(outlet_msg, gensym("DefaultTrackingCaps"), 1, a);
		atom_setfloat(a, (hmd.DisplayRefreshRate));
		outlet_anything(outlet_msg, gensym("DisplayRefreshRate"), 1, a);

		atom_setlong(a, hmd.FirmwareMajor);
		atom_setlong(a + 1, hmd.FirmwareMinor);
		outlet_anything(outlet_msg, gensym("Firmware"), 2, a);

		ovrSizei resolution = hmd.Resolution;
		atom_setlong(a + 0, resolution.w);
		atom_setlong(a + 1, resolution.h);
		outlet_anything(outlet_msg, gensym("resolution"), 2, a);

		// send texture dim (determined via configure()) to the scene jit.gl.node:
		atom_setlong(a + 0, pTextureDim.w);
		atom_setlong(a + 1, pTextureDim.h);
		outlet_anything(outlet_node, _jit_sym_dim, 2, a);
	}

	~oculusrift() {
		// free GL resources created by this external
		dest_closing();
		// disconnect from session
		disconnect();
		// remove from jit.gl* hierarchy
		jit_ob3d_free(this);
		// actually delete object
		max_jit_object_free(this);
	}

	bool textureset_create() {
		post("textureset_create");
		if (!session) return false; 
		if (pTextureSet) return true; // already exists

		// TODO problem here: Jitter API GL headers don't export GL_SRGB8_ALPHA8
		// might also need  GL_EXT_framebuffer_sRGB for the copy
		// "your application should call glEnable(GL_FRAMEBUFFER_SRGB); before rendering into these textures."
		// SDK says:
		// Even though it is not recommended, if your application is configured to treat the texture as a linear 
		// format (e.g.GL_RGBA) and performs linear - to - gamma conversion in GLSL or does not care about gamma - 
		// correction, then:
		// Request an sRGB format(e.g.GL_SRGB8_ALPHA8) swap - texture - set.
		// Do not call glEnable(GL_FRAMEBUFFER_SRGB); when rendering into the swap texture.
		
		auto result = ovr_CreateSwapTextureSetGL(session, GL_RGBA8, pTextureDim.w, pTextureDim.h, &pTextureSet);
		//auto result = ovr_CreateSwapTextureSetGL(session, GL_SRGB8_ALPHA8, pTextureDim.w, pTextureDim.h, &pTextureSet);
		if (result != ovrSuccess) {
			ovrErrorInfo errInfo;
			ovr_GetLastErrorInfo(&errInfo);
			object_error(&ob, "failed to create texture set: %s", errInfo.ErrorString);
			return false;
		}

		

		return true;
	}

	void textureset_destroy() {
		post("textureset_destroy");
		if (session && pTextureSet) {
			ovr_DestroySwapTextureSet(session, pTextureSet);
			pTextureSet = 0;
		}
	}

	/*
	Frame rendering typically involves several steps:
	- obtaining predicted eye poses based on the headset tracking pose, (bang)
	- rendering the view for each eye and, finally, (jit.gl.node @capture 1 renders and then sends texture to this external)
	- submitting eye textures to the compositor through ovr_SubmitFrame. (submit, in response to texture received)
	*/

	void bang() {
		if (!session) {
			// TODO: does SDK provide notification of Rift being reconnected?

			if (reconnect_wait) {
				reconnect_wait--;
			}
			else {
				post("reconnecting...");
				if (!connect()) {
					reconnect_wait = RECONNECTION_TIME;
					
				}
				return;
			}
		}

		t_atom a[6];

		// Query the HMD for the current tracking state.
		// Get both eye poses simultaneously, with IPD offset already included.
		double displayMidpointSeconds = ovr_GetPredictedDisplayTime(session, frameIndex);
		ovrTrackingState ts = ovr_GetTrackingState(session, displayMidpointSeconds, ovrTrue);
		if (ts.StatusFlags & (ovrStatus_OrientationTracked | ovrStatus_PositionTracked)) {
			// get current head pose
			const ovrPosef& pose = ts.HeadPose.ThePose;
			
			// use the tracking state to update the layers (part of how timewarp works)
			ovr_CalcEyePoses(pose, hmdToEyeViewOffset, layer.RenderPose);

			// update the camera view matrices accordingly:
			for (int eye = 0; eye < 2; eye++) {

				// TODO: add navigation pose to this before outputting, or do that in the patcher afterward?

				// modelview
				const ovrVector3f p = layer.RenderPose[eye].Position;
				atom_setfloat(a + 0, p.x);
				atom_setfloat(a + 1, p.y);
				atom_setfloat(a + 2, p.z);
				outlet_anything(outlet_eye[eye], _jit_sym_position, 3, a);

				const ovrQuatf q = layer.RenderPose[eye].Orientation;
				atom_setfloat(a + 0, q.x);
				atom_setfloat(a + 1, q.y);
				atom_setfloat(a + 2, q.z);
				atom_setfloat(a + 3, q.w);
				outlet_anything(outlet_eye[eye], _jit_sym_quat, 4, a);

				// TODO: proj matrix doesn't need to be calculated every frame; only when near/far/layer data changes
				// projection
				const ovrFovPort& fov = layer.Fov[eye];
				atom_setfloat(a + 0, -fov.LeftTan * near_clip);
				atom_setfloat(a + 1, fov.RightTan * near_clip);
				atom_setfloat(a + 2, -fov.DownTan * near_clip);
				atom_setfloat(a + 3, fov.UpTan * near_clip);
				atom_setfloat(a + 4, near_clip);
				atom_setfloat(a + 5, far_clip);
				outlet_anything(outlet_eye[eye], ps_frustum, 6, a);
			}

			// it may be useful to have the pose information in Max for other purposes:
			ovrVector3f p = pose.Position;
			ovrQuatf q = pose.Orientation;

			atom_setfloat(a + 0, p.x);
			atom_setfloat(a + 1, p.y);
			atom_setfloat(a + 2, p.z);
			outlet_anything(outlet_tracking, _jit_sym_position, 3, a);

			atom_setfloat(a + 0, q.x);
			atom_setfloat(a + 1, q.y);
			atom_setfloat(a + 2, q.z);
			atom_setfloat(a + 3, q.w);
			outlet_anything(outlet_tracking, _jit_sym_quat, 4, a);
		}
	}

	// receive a texture
	// TODO: validate texture format?
	void jit_gl_texture(t_symbol * s) {
		intexture = s;
		submit();
	}

	// send the current texture to the Oculus driver:
	void submit() {
		if (!session) return;

		void * texob = jit_object_findregistered(intexture);
		if (!texob) {
			object_error(&ob, "no texture to draw");
			return;	// no texture to copy from.
		}
		// TODO: verify that texob is a texture
		long glid = jit_attr_getlong(texob, ps_glid);
		// get input texture dimensions
		t_atom_long texdim[2];
		jit_attr_getlong_array(texob, _sym_dim, 2, texdim);
		//post("submit texture id %ld dim %ld %ld\n", glid, texdim[0], texdim[1]);

		if (!fbo1) {
			object_error(&ob, "no fbo yet");
			return;	// no texture to copy from.
		}

		if (!pTextureSet) {
			object_error(&ob, "no texture set yet");
			return;	
		}

		// Increment to use next texture, just before writing
		pTextureSet->CurrentIndex = (pTextureSet->CurrentIndex + 1) % pTextureSet->TextureCount;
		// TODO? Clear and set up render-target.    

		// TODO: move stuff out of here if we can:
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo1);
		ovrGLTexture* tex = (ovrGLTexture*)&pTextureSet->Textures[pTextureSet->CurrentIndex];
		GLuint dstId = tex->OGL.TexId;
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, dstId, 0);
		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rbo);
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, pTextureDim.w, pTextureDim.h);
		// following shouldn't be necessary so long as the texture has matching dimensions:
		glBindTexture(GL_TEXTURE_2D, dstId);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (!fbo_check()) {
			object_error(&ob, "falied to create FBO");
			glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
			return;
		}

		glViewport(0, 0, pTextureDim.w, pTextureDim.h);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // TODO -- do we even need a depth buffer?

		// TODO are all these necessary?
		//glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-1.0, 1., 1.0, -1., -1.0, 1.0);

		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		//-------------------------
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_LIGHTING);


		glEnable(GL_TEXTURE_RECTANGLE_ARB);
		glBindTexture(GL_TEXTURE_RECTANGLE_ARB, glid);
		//glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		//glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

		// render quad:
		glBegin(GL_QUADS);
		glTexCoord2i(0, 0);
		glVertex2d(-1., -1.);
		glTexCoord2i(texdim[0], 0);
		glVertex2d(1., -1.);
		glTexCoord2i(texdim[0], texdim[1]);
		glVertex2d(1., 1.);
		glTexCoord2i(0, texdim[1]);
		glVertex2d(-1., 1.);
		glEnd();

		glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

		//glPopClientAttrib();

		//glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);

		
		// Submit frame with one layer we have.
		// ovr_SubmitFrame returns once frame present is queued up and the next texture slot in the ovrSwapTextureSet is available for the next frame. 
		ovrLayerHeader* layers = &layer.Header;
		ovrResult       result = ovr_SubmitFrame(session, frameIndex, nullptr, &layers, 1);
		if (result == ovrError_DisplayLost) {
			/*
			TODO: If you receive ovrError_DisplayLost, the device was removed and the session is invalid.
			Release the shared resources (ovr_DestroySwapTextureSet), destroy the session (ovr_Destory),
			recreate it (ovr_Create), and create new resources (ovr_CreateSwapTextureSetXXX).
			The application's existing private graphics resources do not need to be recreated unless
			the new ovr_Create call returns a different GraphicsLuid.
			*/
			object_error(&ob, "fatal error connection lost.");

			disconnect();
		} 

		frameIndex++;

		// copy mirrorTexture back, or just pass input texture through
		// TODO: implement copying mirror texture back to Jitter


		// TODO: move stuff out of here if we can:
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo1);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, glid, 0);
		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rbo);
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, texdim[0], texdim[1]);
		// following shouldn't be necessary so long as the texture has matching dimensions:
		glBindTexture(GL_TEXTURE_2D, glid);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (!fbo_check()) {
			object_error(&ob, "falied to create FBO");
			glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
			return;
		}

		glViewport(0, 0, texdim[0], texdim[1]);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // TODO -- do we even need a depth buffer?

		// TODO are all these necessary?
		//glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-1.0, 1., 1.0, -1., -1.0, 1.0);

		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		//-------------------------
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_LIGHTING);


		glEnable(GL_TEXTURE_RECTANGLE_ARB);

		tex = (ovrGLTexture*)mirrorTexture;
		ovrSizei mirrorTexDim = mirrorTexture->Header.TextureSize;
		glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex->OGL.TexId); // is this GL_TEXTURE_2D?

		//glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		//glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

		// render quad:
		glBegin(GL_QUADS);
		glTexCoord2i(0, 0);
		glVertex2d(-1., -1.);
		glTexCoord2i(mirrorTexDim.w, 0);
		glVertex2d(1., -1.);
		glTexCoord2i(mirrorTexDim.w, mirrorTexDim.h);
		glVertex2d(1., 1.);
		glTexCoord2i(0, mirrorTexDim.h);
		glVertex2d(-1., 1.);
		glEnd();

		glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

		//glPopClientAttrib();

		//glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);



		t_atom a[1];
		atom_setsym(a, intexture);
		outlet_anything(outlet_tex, ps_jit_gl_texture, 1, a);
	}

	void perf() {
		// just toggle through the various perf modes
		// see https://developer.oculus.com/documentation/pcsdk/latest/concepts/dg-hud/
		perfMode = (perfMode + 1) % ovrPerfHud_Count;
		ovr_SetInt(session, OVR_PERF_HUD_MODE, perfMode);
	}


	t_jit_err draw() {
		// this gets called when the jit.gl.render context updates clients
		// the oculusrift object doesn't draw to the main scene, so there's nothing needed to be done here
		return JIT_ERR_NONE;
	}

	t_jit_err dest_changed() {
		object_post(&ob, "dest_changed");

		glGenFramebuffersEXT(1, &fbo1);
		glGenRenderbuffersEXT(1, &rbo);

		// create a jit.gl.texture to copy mirror to
		// create fbo to do that copy??
		
		return JIT_ERR_NONE;
	}

	// free any locally-allocated GL resources
	t_jit_err dest_closing() {
		object_post(&ob, "dest_closing");
		disconnect();

		if (fbo1) {
			glDeleteFramebuffersEXT(1, &fbo1);
			fbo1 = 0;
		}
		if (rbo) {
			glDeleteRenderbuffersEXT(1, &rbo);
			rbo = 0;
		}

		return JIT_ERR_NONE;
	}

	t_jit_err ui(t_line_3d *p_line, t_wind_mouse_info *p_mouse) {
		/*
		 post("line (%f,%f,%f)-(%f,%f,%f); mouse(%s)",
		 p_line->u[0], p_line->u[1], p_line->u[2],
		 p_line->v[0], p_line->v[1], p_line->v[2],
		 p_mouse->mousesymbol->s_name			// mouse, mouseidle
		 );
		 */
		return JIT_ERR_NONE;
	}



	bool fbo_check() {
		GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
		if (status != GL_FRAMEBUFFER_COMPLETE_EXT) {
			if (status == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT) {
				object_error(&ob, "failed to create render to texture target GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT");
			}
			else if (status == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT) {
				object_error(&ob, "failed to create render to texture target GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS");
			}
			else if (status == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_EXT) {
				object_error(&ob, "failed to create render to texture target GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT");
			}
			else if (status == GL_FRAMEBUFFER_UNSUPPORTED_EXT) {
				object_error(&ob, "failed to create render to texture target GL_FRAMEBUFFER_UNSUPPORTED");
			}
			else {
				object_error(&ob, "failed to create render to texture target %d", status);
			}
			return false;
		}
		return true;
	}

	void mirror_create() {
		if (session && !mirrorTexture) {
			// TODO SRGB?
			auto result = ovr_CreateMirrorTextureGL(session, GL_SRGB8_ALPHA8, pTextureDim.w, pTextureDim.h, &mirrorTexture);
			if (result != ovrSuccess) {
				ovrErrorInfo errInfo;
				ovr_GetLastErrorInfo(&errInfo);
				object_error(&ob, "failed to create mirror texture: %s", errInfo.ErrorString);

				// Sample texture access:
				//ovrGLTexture* tex = (ovrGLTexture*)mirrorTexture;
				//glBindTexture(GL_TEXTURE_2D, tex->OGL.TexId);
				//...
				//glBindTexture(GL_TEXTURE_2D, 0);
			}
		}
	}

	void mirror_destroy() {
		if (session) {
			if (mirrorTexture) {
				ovr_DestroyMirrorTexture(session, mirrorTexture);
				mirrorTexture = 0;
			}
		}
	}
};

void * oculusrift_new(t_symbol *s, long argc, t_atom *argv) {
	oculusrift *x = NULL;
	if ((x = (oculusrift *)object_alloc(max_class))) {// get context:
		t_symbol * dest_name = atom_getsym(argv);
		
		x = new (x)oculusrift(dest_name);
		
		// apply attrs:
		attr_args_process(x, (short)argc, argv);
		
		// invoke any initialization after the attrs are set from here:
		x->connect();
	}
	return (x);
}

void oculusrift_free(oculusrift *x) {
	x->~oculusrift();
}

t_jit_err oculusrift_draw(oculusrift * x) { return x->draw(); }
t_jit_err oculusrift_ui(oculusrift * x, t_line_3d *p_line, t_wind_mouse_info *p_mouse) { return x->ui(p_line, p_mouse); }
t_jit_err oculusrift_dest_closing(oculusrift * x) { return x->dest_closing(); }
t_jit_err oculusrift_dest_changed(oculusrift * x) { return x->dest_changed(); }

void oculusrift_assist(oculusrift *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { // inlet
		sprintf(s, "bang to update tracking, texture to submit, other messages");
	} else {	// outlet
		switch (a) {
		case 0: sprintf(s, "output/mirror texture"); break;
		case 1: sprintf(s, "to left eye camera"); break;
		case 2: sprintf(s, "to right eye camera"); break;
		case 3: sprintf(s, "to scene node"); break;
		case 4: sprintf(s, "tracking state"); break;
		case 5: sprintf(s, "other messages"); break;
		//default: sprintf(s, "I am outlet %ld", a); break;
		}
	}
}

void oculusrift_connect(oculusrift * x) {
	x->connect();
}

void oculusrift_disconnect(oculusrift * x) {
	x->disconnect();
}

void oculusrift_configure(oculusrift * x) {
	x->configure();
}

void oculusrift_info(oculusrift * x) {
	x->info();
}

void oculusrift_bang(oculusrift * x) {
	x->bang();
}

void oculusrift_submit(oculusrift * x) {
	x->submit();
}

void oculusrift_perf(oculusrift * x) {
	x->perf();
}

void oculusrift_recenter(oculusrift * x) {
	if (x->session) ovr_RecenterPose(x->session);
}

t_max_err oculusrift_pixel_density_set(oculusrift *x, t_object *attr, long argc, t_atom *argv) {
	x->pixel_density = atom_getfloat(argv);

	x->configure();
	return 0;
}

t_max_err oculusrift_max_fov_set(oculusrift *x, t_object *attr, long argc, t_atom *argv) {
	x->max_fov = atom_getlong(argv);

	x->configure();
	return 0;
}

void oculusrift_jit_gl_texture(oculusrift * x, t_symbol * s, long argc, t_atom * argv) {
	if (argc > 0 && atom_gettype(argv) == A_SYM) {
		x->jit_gl_texture(atom_getsym(argv));
	}
}

// Application Loop:
//  - Call ovr_GetPredictedDisplayTime() to get the current frame timing information.
//  - Call ovr_GetTrackingState() and ovr_CalcEyePoses() to obtain the predicted
//    rendering pose for each eye based on timing.
//  - Increment ovrTextureSet::CurrentIndex for each layer you will be rendering to 
//    in the next step.
//  - Render the scene content into ovrTextureSet::CurrentIndex for each eye and layer
//    you plan to update this frame. 
//  - Call ovr_SubmitFrame() to render the distorted layers to the back buffer
//    and present them on the HMD. If ovr_SubmitFrame returns ovrSuccess_NotVisible,
//    there is no need to render the scene for the next loop iteration. Instead,
//    just call ovr_SubmitFrame again until it returns ovrSuccess. ovrTextureSet::CurrentIndex 
//    for each layer should refer to the texure you want to display.
//

void oculusrift_quit() {
	ovr_Shutdown();
}

void oculusrift_log(int level, const char* message) {
	post("oculus log %d %s", level, message);
}

void ext_main(void *r)
{
	t_class *c;
	ovrResult result;

	common_symbols_init();
	ps_quat = gensym("quat");
	ps_pos = gensym("pos");
	ps_viewport = gensym("viewport");
	ps_frustum = gensym("frustum");
	ps_warning = gensym("warning");
	ps_glid = gensym("glid");
	ps_jit_gl_texture = gensym("jit_gl_texture");

	// init OVR SDK
	result = ovr_Initialize(NULL);
	if (OVR_FAILURE(result)) {
		error( "LibOVR: failed to initialize library");
		switch (result) {
			case ovrError_Initialize: object_error(NULL, "Generic initialization error."); break;
			case ovrError_LibLoad: object_error(NULL, "Couldn't load LibOVRRT."); break;
			case ovrError_LibVersion: object_error(NULL, "LibOVRRT version incompatibility."); break;
			case ovrError_ServiceConnection: object_error(NULL, "Couldn't connect to the OVR Service."); break;
			case ovrError_ServiceVersion: object_error(NULL, "OVR Service version incompatibility."); break;
			case ovrError_IncompatibleOS: object_error(NULL, "The operating system version is incompatible."); break;
			case ovrError_DisplayInit: object_error(NULL, "Unable to initialize the HMD display."); break;
			case ovrError_ServerStart:  object_error(NULL, "Unable to start the server. Is it already running?"); break;
			case ovrError_Reinitialization: object_error(NULL, "Attempted to re-initialize with a different version."); break;
			default: object_error(NULL, "unknown initialization error."); break;
		}
		return;

		/*
		// was crashy:
		ovrErrorInfo errInfo;
		ovr_GetLastErrorInfo(&errInfo);
		object_error(NULL, errInfo.ErrorString);
		*/
	}
	quittask_install((method)oculusrift_quit, NULL);
	
	c = class_new("oculusrift", (method)oculusrift_new, (method)oculusrift_free, (long)sizeof(oculusrift),
				  0L /* leave NULL!! */, A_GIMME, 0);
	
	long ob3d_flags = JIT_OB3D_NO_MATRIXOUTPUT | JIT_OB3D_DOES_UI;
	/*
	 JIT_OB3D_NO_ROTATION_SCALE;
	 ob3d_flags |= JIT_OB3D_NO_POLY_VARS;
	 ob3d_flags |= JIT_OB3D_NO_BLEND;
	 ob3d_flags |= JIT_OB3D_NO_TEXTURE;
	 ob3d_flags |= JIT_OB3D_NO_MATRIXOUTPUT;
	 ob3d_flags |= JIT_OB3D_AUTO_ONLY;
	 ob3d_flags |= JIT_OB3D_NO_DEPTH;
	 ob3d_flags |= JIT_OB3D_NO_ANTIALIAS;
	 ob3d_flags |= JIT_OB3D_NO_FOG;
	 ob3d_flags |= JIT_OB3D_NO_LIGHTING_MATERIAL;
	 ob3d_flags |= JIT_OB3D_NO_SHADER;
	 ob3d_flags |= JIT_OB3D_NO_BOUNDS;
	 ob3d_flags |= JIT_OB3D_NO_COLOR;
	 */
	
	void * ob3d = jit_ob3d_setup(c, calcoffset(oculusrift, ob3d), ob3d_flags);
	// define our OB3D draw methods
	//jit_class_addmethod(c, (method)(oculusrift_draw), "ob3d_draw", A_CANT, 0L);
	jit_class_addmethod(c, (method)(oculusrift_dest_closing), "dest_closing", A_CANT, 0L);
	jit_class_addmethod(c, (method)(oculusrift_dest_changed), "dest_changed", A_CANT, 0L);
	if (ob3d_flags & JIT_OB3D_DOES_UI) {
		jit_class_addmethod(c, (method)(oculusrift_ui), "ob3d_ui", A_CANT, 0L);
	}
	// must register for ob3d use
	jit_class_addmethod(c, (method)jit_object_register, "register", A_CANT, 0L);
	

	
	/* you CAN'T call this from the patcher */
	class_addmethod(c, (method)oculusrift_assist,			"assist",		A_CANT, 0);


	class_addmethod(c, (method)oculusrift_jit_gl_texture, "jit_gl_texture", A_GIMME, 0);

	class_addmethod(c, (method)oculusrift_connect, "connect", 0);
	class_addmethod(c, (method)oculusrift_disconnect, "disconnect", 0);
	class_addmethod(c, (method)oculusrift_configure, "configure", 0);
	class_addmethod(c, (method)oculusrift_info, "info", 0);


	class_addmethod(c, (method)oculusrift_recenter, "recenter", 0);

	class_addmethod(c, (method)oculusrift_bang, "bang", 0);
	class_addmethod(c, (method)oculusrift_submit, "submit", 0);
	class_addmethod(c, (method)oculusrift_perf, "perf", 0);

	CLASS_ATTR_FLOAT(c, "near_clip", 0, oculusrift, near_clip);
	CLASS_ATTR_FLOAT(c, "far_clip", 0, oculusrift, far_clip);

	CLASS_ATTR_FLOAT(c, "pixel_density", 0, oculusrift, pixel_density);
	CLASS_ATTR_ACCESSORS(c, "pixel_density", NULL, oculusrift_pixel_density_set);

	// TODO: why is Rift not using max FOV (seems like the black overlay is not being made bigger - oculus bug?)
	CLASS_ATTR_LONG(c, "max_fov", 0, oculusrift, max_fov);
	CLASS_ATTR_ACCESSORS(c, "max_fov", NULL, oculusrift_max_fov_set);

	
	class_register(CLASS_BOX, c);
	max_class = c;
}
