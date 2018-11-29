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

#ifdef __GNUC__
#include <stdint.h>
#else
#include "stdint.h"
#endif

#include <Ole2.h>
typedef OLECHAR* WinStr;

// how many glm headers do we really need?
#define GLM_FORCE_RADIANS
#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/gtc/matrix_access.hpp"
#include "glm/gtc/matrix_inverse.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/noise.hpp"
#include "glm/gtc/random.hpp"
#include "glm/gtc/type_ptr.hpp"

using glm::vec2;
using glm::vec3;
using glm::vec4;
using glm::quat;


// jitter uses xyzw format
// glm:: uses wxyz format
// xyzw -> wxyz
template<typename T, glm::precision P>
inline glm::detail::tquat<T, P> quat_from_jitter(glm::detail::tquat<T, P> const & v) {
	return glm::detail::tquat<T, P>(v.z, v.w, v.x, v.y);
}

// wxyz -> xyzw
template<typename T, glm::precision P>
inline glm::detail::tquat<T, P> quat_to_jitter(glm::detail::tquat<T, P> const & v) {
	return glm::detail::tquat<T, P>(v.x, v.y, v.z, v.w);
}

// TODO: is this necessary, or can I use GLM?
// there's glm::gtx::quaternion::rotate, but it takes a vec4

//	q must be a normalized quaternion
template<typename T, glm::precision P>
glm::detail::tvec3<T, P> & quat_rotate(glm::detail::tquat<T, P> const & q, glm::detail::tvec3<T, P> & v) {
	// qv = vec4(v, 0) // 'pure quaternion' derived from vector
	// return ((q * qv) * q^-1).xyz
	// reduced:
	vec4 p;
	p.x = q.w*v.x + q.y*v.z - q.z*v.y;	// x
	p.y = q.w*v.y + q.z*v.x - q.x*v.z;	// y
	p.z = q.w*v.z + q.x*v.y - q.y*v.x;	// z
	p.w = -q.x*v.x - q.y*v.y - q.z*v.z;	// w

	v.x = p.x*q.w - p.w*q.x + p.z*q.y - p.y*q.z;	// x
	v.y = p.y*q.w - p.w*q.y + p.x*q.z - p.z*q.x;	// y
	v.z = p.z*q.w - p.w*q.z + p.y*q.x - p.x*q.y;	// z

	return v;
}

// equiv. quat_rotate(quat_conj(q), v):
// q must be a normalized quaternion
template<typename T, glm::precision P>
void quat_unrotate(glm::detail::tquat<T, P> const & q, glm::detail::tvec3<T, P> & v) {
	// return quat_mul(quat_mul(quat_conj(q), vec4(v, 0)), q).xyz;
	// reduced:
	vec4 p;
	p.x = q.w*v.x + q.y*v.z - q.z*v.y;	// x
	p.y = q.w*v.y + q.z*v.x - q.x*v.z;	// y
	p.z = q.w*v.z + q.x*v.y - q.y*v.x;	// z
	p.w = q.x*v.x + q.y*v.y + q.z*v.z;	// -w

	v.x = p.w*q.x + p.x*q.w + p.y*q.z - p.z*q.y;  // x
	v.x = p.w*q.y + p.y*q.w + p.z*q.x - p.x*q.z;  // y
	v.x = p.w*q.z + p.z*q.w + p.x*q.y - p.y*q.x;   // z
}


template <typename celltype>
struct jitmat {

	t_object * mat;
	t_symbol * sym;
	t_atom name[1];
	int w, h;
	vec2 dim;

	celltype * back;

	jitmat() {
		mat = 0;
		back = 0;
		sym = 0;
	}

	~jitmat() {
		//if (mat) object_release(mat);
	}

	void init(int planecount, t_symbol * type, int width, int height) {
		w = width;
		h = height;
		t_jit_matrix_info info;
		jit_matrix_info_default(&info);
		info.planecount = planecount;
		info.type = type;
		info.dimcount = 2;
		info.dim[0] = w;
		info.dim[1] = h;
		info.flags |= JIT_MATRIX_DATA_PACK_TIGHT;
		mat = (t_object *)jit_object_new(_jit_sym_jit_matrix, &info);
		jit_object_method(mat, _jit_sym_clear);
		sym = jit_symbol_unique();
		jit_object_method(mat, _jit_sym_getdata, &back);
		mat = (t_object *)jit_object_method(mat, _jit_sym_register, sym);
		atom_setsym(name, sym);

		dim = vec2(w, h);
	}

	celltype read_clamp(int x, int y) {
		x = x < 0 ? 0 : x >= w ? w - 1 : x;
		y = y < 0 ? 0 : y >= h ? h - 1 : y;
		return back[x + y*w];
	}

	celltype mix(celltype a, celltype b, float f) {
		return a + f*(b - a);
	}

	celltype sample(vec2 texcoord) {
		vec2 t = texcoord*(dim - 1.f);
		vec2 t0 = vec2(floor(t.x), floor(t.y));
		vec2 t1 = t0 + 1.f;
		vec2 ta = t - t0;
		celltype v00 = read_clamp(t0.x, t0.y);
		celltype v01 = read_clamp(t1.x, t0.y);
		celltype v10 = read_clamp(t0.x, t1.y);
		celltype v11 = read_clamp(t1.x, t1.y);
		return mix(mix(v00, v01, ta.x), mix(v10, v11, ta.x), ta.y);
	}
};


// unstable extensions
#include "glm/gtx/norm.hpp"

#include "NuiApi.h"
#include <Ole2.h>
#include <algorithm>
#include <new>


#define KINECT_DEPTH_WIDTH 640
#define KINECT_DEPTH_HEIGHT 480

typedef OLECHAR* WinStr;

#pragma pack(push, 1)
struct BGRA {
	unsigned char b, g, r, a;
};
struct ARGB {
	unsigned char a, r, g, b;
};
struct RGB {
	unsigned char r, g, b;
};
#pragma pack(pop)

t_symbol * ps_accel = 0;
t_symbol * ps_floor_plane = 0;
t_symbol * ps_hip_center = 0;
t_symbol * ps_spine = 0;
t_symbol * ps_shoulder_center = 0;
t_symbol * ps_head = 0;
t_symbol * ps_shoulder_left = 0;
t_symbol * ps_elbow_left = 0;
t_symbol * ps_wrist_left = 0;
t_symbol * ps_hand_left = 0;
t_symbol * ps_shoulder_right = 0;
t_symbol * ps_elbow_right = 0;
t_symbol * ps_wrist_right = 0;
t_symbol * ps_hand_right = 0;
t_symbol * ps_hip_left = 0;
t_symbol * ps_knee_left = 0;
t_symbol * ps_ankle_left = 0;
t_symbol * ps_foot_left = 0;
t_symbol * ps_hip_right = 0;
t_symbol * ps_knee_right = 0;
t_symbol * ps_ankle_right = 0;
t_symbol * ps_foot_right = 0;

static t_class * this_class = NULL;


struct t_kinect {
public:
	t_object ob; // max objkinectt, must be first!

	void * outlet_msg;
	void * outlet_skeleton;
	void * outlet_rgb;
	void * outlet_depth;
	void * outlet_player;
	void * outlet_cloud;

	// attrs
	int unique, usecolor, align_depth_to_color, uselock;
	int player, skeleton, seated, near_mode, audio, high_quality_color;
	int skeleton_smoothing;
	int device_count;
	int timeout;
	vec2 rgb_focal, rgb_center;
	vec2 rgb_radial, rgb_tangential;
	vec3 position;
	vec4 orientation;
	quat orientation_glm;
	t_symbol * serial;

	// jit.matrix
	jitmat<uint32_t> depth_mat;
	jitmat<char> player_mat;
	jitmat<RGB> rgb_mat;
	jitmat<vec3> cloud_mat;
	jitmat<vec2> rectify_mat, tmp_mat;

	jitmat<vec3> skel_mat;

	// Current Kinect
	INuiSensor* device;
	WinStr name;
	HANDLE colorStreamHandle;
	HANDLE depthStreamHandle;
	NUI_SKELETON_FRAME skeleton_back;
	t_systhread capture_thread;
	t_systhread_mutex depth_mutex;
	int capturing;

	// calibration
	int hasColorMap;
	long* colorCoordinates;
	uint16_t* mappedDepthTmp;
	uint16_t* unmappedDepthTmp;

	int new_depth_data, new_rgb_data;


	t_kinect() {
		outlet_msg = outlet_new(&ob, 0);
		outlet_skeleton = outlet_new(&ob, 0);
		outlet_player = outlet_new(&ob, "jit_matrix");
		outlet_rgb = outlet_new(&ob, "jit_matrix");
		outlet_depth = outlet_new(&ob, "jit_matrix");
		outlet_cloud = outlet_new(&ob, "jit_matrix");

		unique = 1;
		usecolor = 1;
		uselock = 1;
		align_depth_to_color = 1;
		player = 0;
		skeleton = 0;
		skeleton_smoothing = 1;
		seated = 0;
		near_mode = 0;

		timeout = 30;

		device = 0;
		colorStreamHandle = 0;
		capturing = 0;
		systhread_mutex_new(&depth_mutex, 0);


		new_depth_data = 0;
		new_rgb_data = 0;
		hasColorMap = 0;
		orientation.w = 1;

		HRESULT result = NuiGetSensorCount(&device_count);
		if (result != S_OK) error("failed to get sensor count");
		else post("%d devices", device_count);

		depth_mat.init(1, _jit_sym_long, KINECT_DEPTH_WIDTH, KINECT_DEPTH_HEIGHT);
		player_mat.init(1, _jit_sym_char, KINECT_DEPTH_WIDTH, KINECT_DEPTH_HEIGHT);
		rgb_mat.init(3, _jit_sym_char, KINECT_DEPTH_WIDTH, KINECT_DEPTH_HEIGHT);
		cloud_mat.init(3, _jit_sym_float32, KINECT_DEPTH_WIDTH, KINECT_DEPTH_HEIGHT);
		rectify_mat.init(2, _jit_sym_float32, KINECT_DEPTH_WIDTH, KINECT_DEPTH_HEIGHT);
		tmp_mat.init(2, _jit_sym_float32, KINECT_DEPTH_WIDTH, KINECT_DEPTH_HEIGHT);

		skel_mat.init(3, _jit_sym_float32, 4, 5);

		colorCoordinates = new long[KINECT_DEPTH_WIDTH*KINECT_DEPTH_HEIGHT * 2];
		mappedDepthTmp = new uint16_t[KINECT_DEPTH_WIDTH*KINECT_DEPTH_HEIGHT];
		unmappedDepthTmp = new uint16_t[KINECT_DEPTH_WIDTH*KINECT_DEPTH_HEIGHT];
	}

	~t_kinect() {
		close();
		systhread_mutex_free(depth_mutex);
	}

	// returns delta to apply to v (where v is in snorm):
	vec2 displace(vec2 v, vec3 k, vec2 p) {
		float x = v.x, y = v.y;
		float r2 = x*x + y*y;
		float r4 = r2*r2, r6 = r2*r2*r2;
		float radial = k.x*r2 + k.y*r4 + k.z*r6;
		return vec2(
			x*radial + 2 * p.x*x*y + p.y*(r2 + 2 * x*x),
			y*radial + 2 * p.y*x*y + p.x*(r2 + 2 * y*y)
		);
	}

	void clear() {
		for (int y = 0, i = 0; y<480; y++) {
			for (int x = 0; x<640; x++, i++) {
				rectify_mat.back[i] = tmp_mat.back[i] = vec2(0, 0);
			}
		}
	}

	void close() {
		if (capturing) {
			capturing = 0;
			unsigned int ret;
			long result = systhread_join(capture_thread, &ret);
			post("thread closed");
		}
		else {
			shutdown();
		}

		hasColorMap = 0;
	}

	void open(long argc, t_atom * argv) {
		t_atom a[1];

		if (device) {
			object_warn(&ob, "device already opened");
			return;
		}

		INuiSensor* dev;
		HRESULT result = 0;
		if (argc > 0) {
			if (atom_gettype(argv) == A_SYM) {
				OLECHAR instanceName[100];
				char * s = atom_getsym(argv)->s_name;
				mbstowcs(instanceName, s, strlen(s) + 1);
				result = NuiCreateSensorById(instanceName, &dev);
			}
			else {
				int index = atom_getlong(argv);
				result = NuiCreateSensorByIndex(index, &dev);
			}
		}
		else {
			result = NuiCreateSensorByIndex(0, &dev);
		}
		if (result != S_OK) {
			if (E_NUI_DEVICE_IN_USE == result) {
				error("Kinect for Windows already in use.");
			}
			else if (E_NUI_NOTGENUINE == result) {
				error("Kinect for Windows is not genuine.");
			}
			else if (E_NUI_INSUFFICIENTBANDWIDTH == result) {
				error("Insufficient bandwidth.");
			}
			else if (E_NUI_NOTSUPPORTED == result) {
				error("Kinect for Windows device not supported.");
			}
			else if (E_NUI_NOTCONNECTED == result) {
				error("Kinect for Windows is not connected.");
			}
			else if (E_NUI_NOTREADY == result) {
				error("Kinect for Windows is not ready.");
			}
			else if (E_NUI_NOTPOWERED == result) {
				error("Kinect for Windows is not powered.");
			}
			else if (E_NUI_DATABASE_NOT_FOUND == result) {
				error("Kinect for Windows database not found.");
			}
			else if (E_NUI_DATABASE_VERSION_MISMATCH == result) {
				error("Kinect for Windows database version mismatch.");
			}
			else {
				error("Kinect for Windows could not initialize.");
			}
			return;
		}

		result = dev->NuiStatus();
		switch (result) {
		case S_OK:
			break;
		case S_NUI_INITIALIZING:
			object_post(&ob, "the device is connected, but still initializing"); return;
		case E_NUI_NOTCONNECTED:
			object_error(&ob, "the device is not connected"); return;
		case E_NUI_NOTGENUINE:
			object_post(&ob, "the device is not a valid kinect"); break;
		case E_NUI_NOTSUPPORTED:
			object_post(&ob, "the device is not a supported model"); break;
		case E_NUI_INSUFFICIENTBANDWIDTH:
			object_error(&ob, "the device is connected to a hub without the necessary bandwidth requirements."); return;
		case E_NUI_NOTPOWERED:
			object_post(&ob, "the device is connected, but unpowered."); return;
		default:
			object_post(&ob, "the device has some unspecified error"); return;
		}

		WinStr wstr = dev->NuiDeviceConnectionId();
		std::mbstate_t state = std::mbstate_t();
		int len = 1 + std::wcsrtombs((char *)nullptr, (const wchar_t **)&wstr, 0, &state);
		char outname[128];
		std::wcsrtombs(outname, (const wchar_t **)&wstr, len, &state);
		serial = gensym(outname);

		device = dev;
		post("init device %s", outname);
		atom_setsym(a, serial);
		outlet_anything(outlet_msg, gensym("serial"), 1, a);

		hasColorMap = 0;
		long priority = 10; // maybe increase?
		if (systhread_create((method)&capture_threadfunc, this, 0, priority, 0, &capture_thread)) {
			object_error(&ob, "Failed to create capture thread.");
			capturing = 0;
			close();
			return;
		}
	}

	static void *capture_threadfunc(void *arg) {
		t_kinect *x = (t_kinect *)arg;
		x->run();
		systhread_exit(NULL);
		return NULL;
	}

	void run() {
		HRESULT result = 0;
		DWORD dwImageFrameFlags;
		DWORD initFlags = 0;

		initFlags = 0;
		if (usecolor) initFlags |= NUI_INITIALIZE_FLAG_USES_COLOR;
		if (player) {
			initFlags |= NUI_INITIALIZE_FLAG_USES_DEPTH_AND_PLAYER_INDEX;
		}
		else {
			initFlags |= NUI_INITIALIZE_FLAG_USES_DEPTH;
		}
		if (skeleton) {
			initFlags |= NUI_INITIALIZE_FLAG_USES_SKELETON;
		}
		if (audio) {
			initFlags |= NUI_INITIALIZE_FLAG_USES_AUDIO;
		}
		if (high_quality_color) {
			initFlags |= NUI_INITIALIZE_FLAG_USES_HIGH_QUALITY_COLOR;
		}


		result = device->NuiInitialize(initFlags);

		if (result != S_OK) {
			object_error(&ob, "failed to initialize sensor");
			goto done;
		}
		if (skeleton) {
			if (seated) {
				NuiSkeletonTrackingEnable(NULL, NUI_SKELETON_TRACKING_FLAG_ENABLE_SEATED_SUPPORT);
			}
			else {
				NuiSkeletonTrackingEnable(NULL, 0);
			}
		}
		object_post(&ob, "device initialized");

		if (usecolor) {
			dwImageFrameFlags = 0;
			if (near_mode) dwImageFrameFlags |= NUI_IMAGE_STREAM_FLAG_ENABLE_NEAR_MODE;
			result = device->NuiImageStreamOpen(
				NUI_IMAGE_TYPE_COLOR, //NUI_IMAGE_TYPE eImageType,
				NUI_IMAGE_RESOLUTION_640x480, // NUI_IMAGE_RESOLUTION eResolution,
				dwImageFrameFlags,
				2, //DWORD dwFrameLimit,
				0,
				&colorStreamHandle);
			if (result != S_OK) {
				object_error(&ob, "failed to open color stream");
				goto done;
			}
			object_post(&ob, "opened color stream");
		}

		dwImageFrameFlags = 0;
		dwImageFrameFlags |= NUI_IMAGE_STREAM_FLAG_DISTINCT_OVERFLOW_DEPTH_VALUES;
		if (near_mode) dwImageFrameFlags |= NUI_IMAGE_STREAM_FLAG_ENABLE_NEAR_MODE;
		NUI_IMAGE_TYPE eImageType = NUI_IMAGE_TYPE_DEPTH;
		if (player) {
			eImageType = NUI_IMAGE_TYPE_DEPTH_AND_PLAYER_INDEX;
		}
		result = device->NuiImageStreamOpen(
			eImageType,
			NUI_IMAGE_RESOLUTION_640x480, // NUI_IMAGE_RESOLUTION eResolution,
			dwImageFrameFlags,
			2, //DWORD dwFrameLimit,
			0,
			&depthStreamHandle);
		if (result != S_OK) {
			object_error(&ob, "failed to open depth stream");
			goto done;
		}
		object_post(&ob, "opened depth stream");

		//estimateCalibration();

		capturing = 1;
		post("starting processing");
		while (capturing) {

			if (usecolor) processColor();
			processDepth();
			if (skeleton) pollSkeleton();
		}
		post("finished processing");

	done:
		shutdown();
	}

	vec3 realWorldToDepth(const vec3& p) {
		const Vector4 v = { p.x, p.y, p.z, 1.f };
		LONG x = 0;
		LONG y = 0;
		USHORT d = 0;
		NuiTransformSkeletonToDepthImage(v, &x, &y, &d, NUI_IMAGE_RESOLUTION_640x480);
		d >>= 3;
		return vec3(x, y, d * 0.001f);
	}

	vec3 depthToRealWorld(const vec3& p) {
		Vector4 v = NuiTransformDepthImageToSkeleton(
			LONG(p.x),
			LONG(p.y),
			USHORT(p.z),
			NUI_IMAGE_RESOLUTION_640x480
		);

		return vec3(v.x, v.y, v.z);
	}

	vec3& cloudTransform(vec3& p) {

		p -= position;
		quat_rotate(orientation_glm, p);

		return p;
	}

	void estimateCalibration() {
		// Deduce depth focal from depth to world transforms.
		vec3 p = realWorldToDepth(vec3(0.f, 0.f, 1.f));
		float cx = p.x;
		float cy = p.y;
		p = realWorldToDepth(vec3(1.f, 1.f, 1.f));
		float fx = (p.x - cx);
		float fy = -(p.y - cy);
		const float correction_factor = NUI_CAMERA_COLOR_NOMINAL_FOCAL_LENGTH_IN_PIXELS
			/ (NUI_CAMERA_DEPTH_NOMINAL_FOCAL_LENGTH_IN_PIXELS * 2.f);
		// Pixels are square on a Kinect.
		// Image height gets cropped when going from 1280x1024 in 640x480.
		// The ratio remains 2.
		rgb_focal.x = correction_factor * fx;
		rgb_focal.y = rgb_focal.x;
		rgb_center.x = cx;
		rgb_center.y = cy;

		// bunch more stuff at https://github.com/rgbdemo/nestk/blob/master/ntk/camera/kin4win_grabber.cpp which needs opencv

		t_atom a[2];
		atom_setfloat(a + 0, rgb_center.x);
		atom_setfloat(a + 1, rgb_center.y);
		outlet_anything(outlet_msg, gensym("rgb_center"), 2, a);
		atom_setfloat(a + 0, rgb_focal.x);
		atom_setfloat(a + 1, rgb_focal.y);
		outlet_anything(outlet_msg, gensym("rgb_focal"), 2, a);
	}

	void processDepth() {
		if (!device) return;
		DWORD dwMillisecondsToWait = timeout;
		NUI_IMAGE_FRAME imageFrame;

		HRESULT result = device->NuiImageStreamGetNextFrame(depthStreamHandle, dwMillisecondsToWait, &imageFrame);
		if (result == E_NUI_FRAME_NO_DATA) {
			// timeout with no data. bail or continue?
			systhread_sleep(30);
			return;
		}
		else if (FAILED(result)) {
			switch (result) {
			case E_INVALIDARG:
				object_error(&ob, "arg stream error"); break;
			case E_OUTOFMEMORY:
				object_error(&ob, "Ran out of memory"); break;
			case E_NOINTERFACE:
				object_error(&ob, "unsupported"); break;
			case E_ABORT:
				object_error(&ob, "Operation aborted"); break;
			case E_ACCESSDENIED:
				object_error(&ob, "General access denied error"); break;
			case E_POINTER:
				object_error(&ob, "pointer stream error"); break;
			case E_HANDLE:
				object_error(&ob, "invalid handle"); break;
			case E_PENDING:
				object_error(&ob, "The data necessary to complete this operation is not yet available."); break;
			case S_FALSE:
				object_error(&ob, "timeout"); break;

			default:
				object_error(&ob, "stream error %x"); break;
			}
			systhread_sleep(30);
			return;
		}

		INuiFrameTexture * imageTexture = NULL;
		BOOL bNearMode = near_mode;

		result = device->NuiImageFrameGetDepthImagePixelFrameTexture(depthStreamHandle, &imageFrame, &bNearMode, &imageTexture);
		//imageTexture = imageFrame.pFrameTexture;

		// got data; now turn it into jitter 
		if (!imageTexture) {
			post("no data");
			goto ReleaseFrame;
		}
		NUI_LOCKED_RECT LockedRect;

		// Lock the frame data so the Kinect knows not to modify it while we're reading it
		imageTexture->LockRect(0, &LockedRect, NULL, 0);

		// Make sure we've received valid data
		if (LockedRect.Pitch != 0) {

			// convert to Jitter-friendly RGB layout:
			//const uint16_t * src = (const uint16_t *)LockedRect.pBits;
			NUI_DEPTH_IMAGE_PIXEL * src = reinterpret_cast<NUI_DEPTH_IMAGE_PIXEL*>(LockedRect.pBits);
			uint32_t * dst = depth_mat.back;
			char * dstp = player_mat.back;
			static const int cells = KINECT_DEPTH_HEIGHT * KINECT_DEPTH_WIDTH;

			// First generate packed depth values from extended depth values, which include near pixels.
			for (int i = 0; i<cells; i++) {
				unmappedDepthTmp[i] = src[i].depth << NUI_IMAGE_PLAYER_INDEX_SHIFT;
			}

			if (!hasColorMap) {
				// use it to generate the color map:
				device->NuiImageGetColorPixelCoordinateFrameFromDepthPixelFrameAtResolution(
					NUI_IMAGE_RESOLUTION_640x480, //colorResolution,
					NUI_IMAGE_RESOLUTION_640x480, //depthResolution,
					cells,
					unmappedDepthTmp, // depth_d16
					cells * 2,
					colorCoordinates
				);
				//post("generated color map");
				hasColorMap = 1;
			}

			// update orientation:
			orientation_glm.x = orientation.z;
			orientation_glm.y = orientation.w;
			orientation_glm.z = orientation.x;
			orientation_glm.w = orientation.y;

			if (uselock) systhread_mutex_lock(depth_mutex);

			vec2 dim(KINECT_DEPTH_WIDTH, KINECT_DEPTH_HEIGHT - 1);
			vec2 inv_dim_1 = 1.f / (dim - 1.f);

			if (align_depth_to_color)
			{
				//QWriteLocker locker(&that->m_lock);
				//uint16_t* depth_buf = that->m_current_image.rawDepth16bitsRef().ptr<uint16_t>();
				//mapDepthFrameToRgbFrame(src, depth_buf);

				// clean the output buffers:
				std::fill(dst, dst + cells, 0);
				std::fill(dstp, dstp + cells, 0);
				//memset(cloud_mat.back, 0, sizeof(vec3) * cells);

				float inv_rgb_focal_x = 1.f / rgb_focal.x;
				float inv_rgb_focal_y = 1.f / rgb_focal.y;
				vec2 inv_rgb_focal = 1.f / rgb_focal;

				// read through all the color coordinates
				for (int i = 0; i < cells; ++i) {
					// get the corresponding RGB image coordinate for depth pixel i:
					int c = colorCoordinates[i * 2];
					int r = colorCoordinates[i * 2 + 1];
					// idx is for rgb image:
					int idx = r*KINECT_DEPTH_WIDTH + c;

					if (c >= 0 && c < KINECT_DEPTH_WIDTH
						&& r >= 0 && r < KINECT_DEPTH_HEIGHT) {
						// valid location: get the depth value:			
						uint16_t depth_in_mm = src[i].depth;
						// the output depth/cloud images will be aligned to the rgb image:
						vec3& out = cloud_mat.back[idx];
						if (depth_in_mm > 0) {
							// set the output depth value:
							dst[idx] = depth_in_mm;
							//Player index
							dstp[idx] = (char)src[i].playerIndex;

							// derive the output 3D position
							//out = depthToRealWorld(vec3(c, r, depth_in_mm << 3));

							// first, unproject from pixel plane to idealized focal plane (in meters):
							vec2 xy = (vec2(c, r) - rgb_center) * inv_rgb_focal;
							//float xf = (c - rgb_center.x) * inv_rgb_focal_x;
							//float yf = (r - rgb_center.y) * inv_rgb_focal_y;
							float zf = depth_in_mm * 0.001f;

							// TODO: remove RGB lens distortion effects? (rgb_radial, rgb_tangential)
							// for this we would need an undistortion map to be generated
							// apply rectify:
							xy += rectify_mat.sample(xy);

							// project into 3D space by depth:
							vec3 v(xy.x * zf, -xy.y * zf, zf);

							// apply extrinsic:
							out = cloudTransform(v);

						}
						else {
							// invalid depth: fill zero:
							dst[idx] = 0;
							vec3 v(0, 0, 0);
							out = cloudTransform(v);
						}
					}

				}

				/*
				for (int y=0; y<ydim; y++) {
				for (int x=0; x<xdim; x++) {
				int idx1 = x + y*xdim;

				float m = world.mask[idx1];
				land_prev[idx1] *= m;
				}
				}*/
			}
			else
			{

				/*QWriteLocker locker(&that->m_lock);
				uint16_t* depth_buf = that->m_current_image.rawDepth16bitsRef().ptr<uint16_t>();
				cv::Vec2w* depth_to_color_coords = that->m_current_image.depthToRgbCoordsRef().ptr<cv::Vec2w>();
				extractDepthAndColorCoords (src, depth_buf, depth_to_color_coords);
				*/

				// write cells into depth and cloud matrices:
				for (int i = 0, y = 0; y<KINECT_DEPTH_HEIGHT; y++) {
					for (int x = 0; x<KINECT_DEPTH_WIDTH; x++, i++) {
						uint32_t d = src[i].depth;
						dst[i] = d;
						dstp[i] = (char)src[i].playerIndex;
						if (d > 0) {
							vec3 v = depthToRealWorld(vec3(x, y, d << 3));
							cloudTransform(v);
							cloud_mat.back[i] = v;
						}
						else {
							// invalid depth: fill zero:
							vec3 v(0, 0, 0);
							cloud_mat.back[i] = cloudTransform(v);
						}
					}
				}

				// now apply archipelago processing
				for (int i = 0, y = 0; y<KINECT_DEPTH_HEIGHT; y++) {
					for (int x = 0; x<KINECT_DEPTH_WIDTH; x++, i++) {
						if (dst[i] > 0) {
							vec3 out = cloud_mat.back[i];
						}
					}
				}

			}

			if (uselock) systhread_mutex_unlock(depth_mutex);
			new_depth_data = 1;
		}


		// We're done with the texture so unlock it
		imageTexture->UnlockRect(0);

		//cloud_process();
		//local_cloud_process();

	ReleaseFrame:
		// Release the frame
		device->NuiImageStreamReleaseFrame(depthStreamHandle, &imageFrame);

	}

	void processColor() {
		if (!device) return;
		DWORD dwMillisecondsToWait = timeout;
		NUI_IMAGE_FRAME imageFrame;

		HRESULT result = device->NuiImageStreamGetNextFrame(colorStreamHandle, dwMillisecondsToWait, &imageFrame);
		if (result == E_NUI_FRAME_NO_DATA) {
			// timeout with no data. bail or continue?
			return;
		}
		else if (FAILED(result)) {
			switch (result) {
			case E_INVALIDARG:
				object_error(&ob, "arg stream error"); break;
			case E_OUTOFMEMORY:
				object_error(&ob, "Ran out of memory"); break;
			case E_NOINTERFACE:
				object_error(&ob, "unsupported"); break;
			case E_ABORT:
				object_error(&ob, "Operation aborted"); break;
			case E_ACCESSDENIED:
				object_error(&ob, "General access denied error"); break;
			case E_POINTER:
				object_error(&ob, "pointer stream error"); break;
			case E_HANDLE:
				object_error(&ob, "invalid handle"); break;
			case E_PENDING:
				object_error(&ob, "The data necessary to complete this operation is not yet available."); break;
			case S_FALSE:
				object_error(&ob, "timeout"); break;
			default:
				object_error(&ob, "stream error"); break;
			}
			return;
		}

		int newframe = 0;

		// got data; now turn it into jitter 
		//post("frame %d", imageFrame.dwFrameNumber);
		//outlet_int(outlet_msg, imageFrame.dwFrameNumber);
		INuiFrameTexture * imageTexture = imageFrame.pFrameTexture;
		NUI_LOCKED_RECT LockedRect;

		// Lock the frame data so the Kinect knows not to modify it while we're reading it
		imageTexture->LockRect(0, &LockedRect, NULL, 0);

		// Make sure we've received valid data
		if (LockedRect.Pitch != 0) {
			//post("pitch %d size %d", LockedRect.Pitch, LockedRect.size);
			//static_cast<BYTE *>(LockedRect.pBits), LockedRect.size

			//sysmem_copyptr(LockedRect.pBits, rgb_back, LockedRect.size);

			// convert to Jitter-friendly RGB layout:


			const BGRA * src = (const BGRA *)LockedRect.pBits;
			RGB * dst = (RGB *)rgb_mat.back;
			int cells = KINECT_DEPTH_HEIGHT * KINECT_DEPTH_WIDTH;
			if (align_depth_to_color) {
				for (int i = 0; i < cells; ++i) {
					dst[i].r = src[i].r;
					dst[i].g = src[i].g;
					dst[i].b = src[i].b;
				}
			}
			else {
				// align color to depth:
				//std::fill(dst, dst + cells, RGB(0, 0, 0));
				for (int i = 0; i < cells; ++i) {
					int c = colorCoordinates[i * 2];
					int r = colorCoordinates[i * 2 + 1];
					if (c >= 0 && c < KINECT_DEPTH_WIDTH
						&& r >= 0 && r < KINECT_DEPTH_HEIGHT) {
						// valid location: depth value:
						int idx = r*KINECT_DEPTH_WIDTH + c;
						dst[i].r = src[idx].r;
						dst[i].g = src[idx].g;
						dst[i].b = src[idx].b;
					}
				}
			}
			newframe = 1;
		}

		// We're done with the texture so unlock it
		imageTexture->UnlockRect(0);

		//	ReleaseFrame:
		// Release the frame
		device->NuiImageStreamReleaseFrame(colorStreamHandle, &imageFrame);

		//if (newframe) cloud_rgb_process();

		new_rgb_data = 1;
	}

	void pollSkeleton() {
		if (!device) return;

		HRESULT result;
		NUI_SKELETON_FRAME frame = { 0 };
		DWORD dwMillisecondsToWait = timeout;

		result = device->NuiSkeletonGetNextFrame(dwMillisecondsToWait, &frame);
		if (result == E_NUI_FRAME_NO_DATA || result == S_FALSE) {
			// timeout with no data. bail or continue?
			return;
		}
		else if (FAILED(result)) {
			switch (result) {
			case E_POINTER:
				object_error(&ob, "pointer stream error"); break;
			default:
				object_error(&ob, "stream error"); break;
			}
			return;
		}


		// ALSO SEE http://msdn.microsoft.com/en-us/library/jj131024.aspx

		//___________________________________________________________________________
		// Some smoothing with little latency (defaults).
		// Only filters out small jitters.
		// Good for gesture recognition in games.
		const NUI_TRANSFORM_SMOOTH_PARAMETERS defaultParams = { 0.5f, 0.5f, 0.5f, 0.05f, 0.04f };

		// Smoothed with some latency.
		// Filters out medium jitters.
		// Good for a menu system that needs to be smooth but
		// doesn't need the reduced latency as much as gesture recognition does.
		const NUI_TRANSFORM_SMOOTH_PARAMETERS somewhatLatentParams = { 0.5f, 0.1f, 0.5f, 0.1f, 0.1f };

		// Very smooth, but with a lot of latency.
		// Filters out large jitters.
		// Good for situations where smooth data is absolutely required
		// and latency is not an issue.
		const NUI_TRANSFORM_SMOOTH_PARAMETERS verySmoothParams = { 0.7f, 0.3f, 1.0f, 1.0f, 1.0f };

		//Smoothing
		switch (skeleton_smoothing) {
		case 1:
			device->NuiTransformSmooth(&frame, &defaultParams); break;
		case 2:
			device->NuiTransformSmooth(&frame, &somewhatLatentParams); break;
		case 3:
			device->NuiTransformSmooth(&frame, &verySmoothParams); break;
		};

		// success: copy the data:
		memcpy(&skeleton_back, &frame, sizeof(NUI_SKELETON_FRAME));
	}

	void shutdown() {
		if (device) {
			device->NuiShutdown();
			device->Release();
			device = 0;
		}
	}

	void bang() {
		if (usecolor && (new_rgb_data || unique == 0)) {
			outlet_anything(outlet_rgb, _jit_sym_jit_matrix, 1, rgb_mat.name);
			new_rgb_data = 0;
		}
		if (new_depth_data || unique == 0) {
			if (skeleton) outputSkeleton();
			if (player) outlet_anything(outlet_player, _jit_sym_jit_matrix, 1, player_mat.name);
			if (uselock) systhread_mutex_lock(depth_mutex);
			outlet_anything(outlet_depth, _jit_sym_jit_matrix, 1, depth_mat.name);
			outlet_anything(outlet_cloud, _jit_sym_jit_matrix, 1, cloud_mat.name);
			if (uselock) systhread_mutex_unlock(depth_mutex);
			new_depth_data = 0;
		}
	}

	void outputSkeleton() {
		t_atom a[4];
		/*
		Vector4& floorplane = frame.vFloorClipPlane;
		The floor plane is an estimate based on image analysis. This estimate changes initially and then settles over time, unless a condition changes due to such factors as movement of the camera or furniture. The skeleton pipeline uses this plane as a lower clipping plane. The general equation of a plane in homogenous coordinates is Ax + By + Cz + D = 0. The x, y, z, and w members of the Vector4 structure correspond to the coefficients A, B, C, and D, respectively. The camera is always at (0, 0, 0, 1), so the coefficient D is the negative of the height of the camera from the floor, in meters.
		*/
		atom_setfloat(a + 0, skeleton_back.vFloorClipPlane.x);
		atom_setfloat(a + 1, skeleton_back.vFloorClipPlane.y);
		atom_setfloat(a + 2, skeleton_back.vFloorClipPlane.z);
		atom_setfloat(a + 3, skeleton_back.vFloorClipPlane.w);
		outlet_anything(outlet_msg, ps_floor_plane, 4, a);

		/*
		outlet_anything(outlet_skeleton, ps_accel , 4, a);
		outlet_anything(outlet_skeleton, ps_hip_center, 4, a);
		outlet_anything(outlet_skeleton, ps_spine, 4, a);
		outlet_anything(outlet_skeleton, ps_shoulder_center, 4, a);
		outlet_anything(outlet_skeleton, ps_head, 4, a);
		outlet_anything(outlet_skeleton, ps_shoulder_left, 4, a);
		outlet_anything(outlet_skeleton, ps_elbow_left, 4, a);
		outlet_anything(outlet_skeleton, ps_wrist_left, 4, a);
		outlet_anything(outlet_skeleton, ps_hand_left, 4, a);
		outlet_anything(outlet_skeleton, ps_shoulder_right, 4, a);
		outlet_anything(outlet_skeleton, ps_elbow_right, 4, a);
		outlet_anything(outlet_skeleton, ps_wrist_right, 4, a);
		outlet_anything(outlet_skeleton, ps_hand_right, 4, a);
		outlet_anything(outlet_skeleton, ps_shoulder_left, 4, a);
		outlet_anything(outlet_skeleton, ps_elbow_left, 4, a);
		outlet_anything(outlet_skeleton, ps_hip_left, 4, a);
		outlet_anything(outlet_skeleton, ps_knee_left, 4, a);
		outlet_anything(outlet_skeleton, ps_ankle_left, 4, a);
		outlet_anything(outlet_skeleton, ps_foot_left, 4, a);
		outlet_anything(outlet_skeleton, ps_hip_right, 4, a);
		outlet_anything(outlet_skeleton, ps_knee_right, 4, a);
		outlet_anything(outlet_skeleton, ps_ankle_right, 4, a);
		outlet_anything(outlet_skeleton, ps_foot_right, 4, a);
		*/

		for (int i = 0; i < NUI_SKELETON_COUNT; i++) {
			const NUI_SKELETON_DATA & skeleton = skeleton_back.SkeletonData[i];
			uint32_t id = skeleton.dwUserIndex;//skeleton.dwTrackingID;

			switch (skeleton.eTrackingState) {
			case NUI_SKELETON_TRACKED: {
				outputBone(id, ps_hip_center, skeleton, NUI_SKELETON_POSITION_HIP_CENTER);
				outputBone(id, ps_spine, skeleton, NUI_SKELETON_POSITION_SPINE);
				outputBone(id, ps_shoulder_center, skeleton, NUI_SKELETON_POSITION_SHOULDER_CENTER);
				outputBone(id, ps_head, skeleton, NUI_SKELETON_POSITION_HEAD);
				outputBone(id, ps_shoulder_left, skeleton, NUI_SKELETON_POSITION_SHOULDER_LEFT);
				outputBone(id, ps_elbow_left, skeleton, NUI_SKELETON_POSITION_ELBOW_LEFT);
				outputBone(id, ps_wrist_left, skeleton, NUI_SKELETON_POSITION_WRIST_LEFT);
				outputBone(id, ps_hand_left, skeleton, NUI_SKELETON_POSITION_HAND_LEFT);
				outputBone(id, ps_shoulder_right, skeleton, NUI_SKELETON_POSITION_SHOULDER_RIGHT);
				outputBone(id, ps_elbow_right, skeleton, NUI_SKELETON_POSITION_ELBOW_RIGHT);
				outputBone(id, ps_wrist_right, skeleton, NUI_SKELETON_POSITION_WRIST_RIGHT);
				outputBone(id, ps_hand_right, skeleton, NUI_SKELETON_POSITION_HAND_RIGHT);
				outputBone(id, ps_hip_left, skeleton, NUI_SKELETON_POSITION_HIP_LEFT);
				outputBone(id, ps_knee_left, skeleton, NUI_SKELETON_POSITION_KNEE_LEFT);
				outputBone(id, ps_ankle_left, skeleton, NUI_SKELETON_POSITION_ANKLE_LEFT);
				outputBone(id, ps_foot_left, skeleton, NUI_SKELETON_POSITION_FOOT_LEFT);
				outputBone(id, ps_hip_right, skeleton, NUI_SKELETON_POSITION_HIP_RIGHT);
				outputBone(id, ps_knee_right, skeleton, NUI_SKELETON_POSITION_KNEE_RIGHT);
				outputBone(id, ps_ankle_right, skeleton, NUI_SKELETON_POSITION_ANKLE_RIGHT);
				outputBone(id, ps_foot_right, skeleton, NUI_SKELETON_POSITION_FOOT_RIGHT);

				// 20 points
				// 19 joints (4 arm, 4 arm, 4 leg, 4 leg, 3 spine)
				// to make it an even 4x5 matrix friendly for mesh lines, could add a duplicate spine joint (neck)
				// to make it sensible, each should arc out from the center (hip or shoulder)

				Vector4 pos;

				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_HIP_CENTER]; skel_mat.back[0] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_SPINE]; skel_mat.back[1] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_SHOULDER_CENTER]; skel_mat.back[2] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_HEAD]; skel_mat.back[3] = vec3(pos.x, pos.y, pos.z);

				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_SHOULDER_LEFT]; skel_mat.back[4] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_ELBOW_LEFT]; skel_mat.back[5] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_WRIST_LEFT]; skel_mat.back[6] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_HAND_LEFT]; skel_mat.back[7] = vec3(pos.x, pos.y, pos.z);

				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_SHOULDER_RIGHT]; skel_mat.back[8] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_ELBOW_RIGHT]; skel_mat.back[9] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_WRIST_RIGHT]; skel_mat.back[10] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_HAND_RIGHT]; skel_mat.back[11] = vec3(pos.x, pos.y, pos.z);

				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_HIP_LEFT]; skel_mat.back[12] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_KNEE_LEFT]; skel_mat.back[13] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_ANKLE_LEFT]; skel_mat.back[14] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_FOOT_LEFT]; skel_mat.back[15] = vec3(pos.x, pos.y, pos.z);

				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_HIP_RIGHT]; skel_mat.back[16] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_KNEE_RIGHT]; skel_mat.back[17] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_ANKLE_RIGHT]; skel_mat.back[18] = vec3(pos.x, pos.y, pos.z);
				pos = skeleton.SkeletonPositions[NUI_SKELETON_POSITION_FOOT_RIGHT]; skel_mat.back[19] = vec3(pos.x, pos.y, pos.z);

				// output the matrix with player ID
				atom_setlong(a + 0, id);
				atom_setsym(a + 1, _jit_sym_jit_matrix);
				atom_setsym(a + 2, skel_mat.sym);
				outlet_anything(outlet_skeleton, gensym("matrix"), 3, a);
			}
									   break;
			case NUI_SKELETON_POSITION_ONLY:
				//DrawSkeletonPosition(skeleton.Position);
				break;
			}



		}
	}

	void outputBone(uint32_t id, t_symbol * name, const NUI_SKELETON_DATA & skeleton, NUI_SKELETON_POSITION_INDEX k) {
		if (skeleton.eSkeletonPositionTrackingState[k] == NUI_SKELETON_POSITION_NOT_TRACKED) {
			return;
		}

		const Vector4 & pos = skeleton.SkeletonPositions[k];
		t_atom a[5];

		atom_setlong(a + 0, id);
		atom_setsym(a + 1, name);
		atom_setfloat(a + 2, pos.x);
		atom_setfloat(a + 3, pos.y);
		atom_setfloat(a + 4, pos.z);
		outlet_list(outlet_skeleton, NULL, 5, a);

	}

	void accel() {
		Vector4 v;
		HRESULT ret = device->NuiAccelerometerGetCurrentReading(&v);
		if (FAILED(ret)) return;
		t_atom a[3];
		atom_setfloat(a + 0, v.x);
		atom_setfloat(a + 1, v.y);
		atom_setfloat(a + 2, v.z);
		outlet_anything(outlet_msg, ps_accel, 3, a);
	}

	void rectify_matrix(t_symbol * name) {
		void * in_mat = jit_object_findregistered(name);
		if (!in_mat) {
			jit_error_code(&ob, JIT_ERR_INVALID_INPUT);
			return;
		}
		t_jit_matrix_info in_info;
		char * in_bp;

		int lw = KINECT_DEPTH_WIDTH;
		int lh = KINECT_DEPTH_HEIGHT;

		// lock it:
		long in_savelock = (long)jit_object_method(in_mat, _jit_sym_lock, 1);

		// ensure data exists:
		jit_object_method(in_mat, _jit_sym_getdata, &in_bp);
		if (!in_bp) {
			jit_error_code(&ob, JIT_ERR_INVALID_INPUT);
			return;
		}

		// ensure the type is correct:
		jit_object_method(in_mat, _jit_sym_getinfo, &in_info);
		if (in_info.type != _jit_sym_float32) {
			jit_error_code(&ob, JIT_ERR_MISMATCH_TYPE);
			return;
		}
		else if (in_info.planecount != 2) {
			jit_error_code(&ob, JIT_ERR_MISMATCH_PLANE);
			return;
		}
		else if (in_info.dimcount != 2) {
			jit_error_code(&ob, JIT_ERR_MISMATCH_DIM);
			return;
		}
		if (in_info.dim[0] != lw || in_info.dim[1] != lh) {
			jit_error_code(&ob, JIT_ERR_MISMATCH_DIM);
			return;
		}

		// copy data:
		vec2 * input = (vec2 *)in_bp;
		for (int y = 0, i = 0; y < lh; y++) {
			for (int x = 0; x < lw; x++, i++) {
				rectify_mat.back[i] = input[i];
			}
		}

		// restore matrix lock state:
		jit_object_method(in_mat, _jit_sym_lock, in_savelock);
	}
};

void kinect_rectify_matrix(t_kinect * x, t_symbol * s) { x->rectify_matrix(s); }

void kinect_open(t_kinect * x, t_symbol * s, long argc, t_atom * argv) { x->open(argc, argv); }
void kinect_close(t_kinect * x) { x->close(); }
void kinect_bang(t_kinect * x) { x->bang(); }
void kinect_accel(t_kinect *x) { x->accel(); }
void kinect_estimate(t_kinect *x) { x->estimateCalibration(); }
void kinect_free(t_kinect * x) { x->~t_kinect(); }
void kinect_assist(t_kinect *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { // inlet
		if (a == 0) {
			sprintf(s, "messages in, bang to report orientation");
		}
		else {
			sprintf(s, "I am inlet %ld", a);
		}
	}
	else {	// outlet
		if (a == 0) {
			sprintf(s, "3D point cloud (matrix)");
		}
		else if (a == 1) {
			sprintf(s, "depth data (matrix)");
		}
		else if (a == 2) {
			sprintf(s, "RGB data (matrix)");
		}
		else if (a == 3) {
			sprintf(s, "player ID (matrix)");
		}
		else if (a == 3) {
			sprintf(s, "skeleton data (list)");
		}
		else {
			sprintf(s, "messages out");
		}
	}
}


void * kinect_new(t_symbol * s, long argc, t_atom * argv) {
	t_kinect * x = 0;
	if ((x = (t_kinect *)object_alloc(this_class))) {
		x = new (x)t_kinect;
		attr_args_process(x, (short)argc, argv);
	}
	post("k new %p\n", x);
	return x;
}

void ext_main(void *r) {
	if (this_class) return;

	// just a hack to get jitter initialized:
	post("GL: %s", jit_gl_get_version());

	ps_accel = gensym("accel");
	ps_floor_plane = gensym("floor_plane");
	ps_hip_center = gensym("hip_center");
	ps_spine = gensym("spine");
	ps_shoulder_center = gensym("shoulder_center");
	ps_head = gensym("head");
	ps_shoulder_left = gensym("shoulder_left");
	ps_elbow_left = gensym("elbow_left");
	ps_wrist_left = gensym("wrist_left");
	ps_hand_left = gensym("hand_left");
	ps_shoulder_right = gensym("shoulder_right");
	ps_elbow_right = gensym("elbow_right");
	ps_wrist_right = gensym("wrist_right");
	ps_hand_right = gensym("hand_right");
	ps_hip_left = gensym("hip_left");
	ps_knee_left = gensym("knee_left");
	ps_ankle_left = gensym("ankle_left");
	ps_foot_left = gensym("foot_left");
	ps_hip_right = gensym("hip_right");
	ps_knee_right = gensym("knee_right");
	ps_ankle_right = gensym("ankle_right");
	ps_foot_right = gensym("foot_right");

	this_class = class_new("kinect", (method)kinect_new, (method)kinect_free, sizeof(t_kinect), 0L, A_GIMME, 0);

	class_addmethod(this_class, (method)kinect_open, "open", A_GIMME, 0);
	class_addmethod(this_class, (method)kinect_close, "close", 0);
	class_addmethod(this_class, (method)kinect_bang, "bang", 0);

	class_addmethod(this_class, (method)kinect_assist, "assist", A_CANT, 0);
	class_addmethod(this_class, (method)kinect_accel, "accel", 0);
	class_addmethod(this_class, (method)kinect_estimate, "estimate", 0);

	class_addmethod(this_class, (method)kinect_rectify_matrix, "rectify_matrix", A_SYM, 0);

	CLASS_ATTR_SYM(this_class, "serial", 0, t_kinect, serial);

	CLASS_ATTR_LONG(this_class, "timeout", 0, t_kinect, timeout);

	CLASS_ATTR_LONG(this_class, "align_depth_to_color", 0, t_kinect, align_depth_to_color);
	CLASS_ATTR_STYLE(this_class, "align_depth_to_color", 0, "onoff");

	CLASS_ATTR_LONG(this_class, "unique", 0, t_kinect, unique);
	CLASS_ATTR_STYLE(this_class, "unique", 0, "onoff");
	CLASS_ATTR_LONG(this_class, "device_count", 0, t_kinect, device_count);
	CLASS_ATTR_LONG(this_class, "near_mode", 0, t_kinect, near_mode);
	CLASS_ATTR_STYLE(this_class, "near_mode", 0, "onoff");
	CLASS_ATTR_LONG(this_class, "player", 0, t_kinect, player);
	CLASS_ATTR_STYLE(this_class, "player", 0, "onoff");
	CLASS_ATTR_LONG(this_class, "skeleton", 0, t_kinect, skeleton);
	CLASS_ATTR_STYLE(this_class, "skeleton", 0, "onoff");
	CLASS_ATTR_LONG(this_class, "seated", 0, t_kinect, seated);
	CLASS_ATTR_STYLE(this_class, "seated", 0, "onoff");

	CLASS_ATTR_LONG(this_class, "uselock", 0, t_kinect, uselock);
	CLASS_ATTR_STYLE(this_class, "uselock", 0, "onoff");
	CLASS_ATTR_LONG(this_class, "usecolor", 0, t_kinect, usecolor);
	CLASS_ATTR_STYLE(this_class, "usecolor", 0, "onoff");

	CLASS_ATTR_LONG(this_class, "skeleton_smoothing", 0, t_kinect, skeleton_smoothing);

	CLASS_ATTR_FLOAT_ARRAY(this_class, "rgb_focal", 0, t_kinect, rgb_focal, 2);
	CLASS_ATTR_FLOAT_ARRAY(this_class, "rgb_center", 0, t_kinect, rgb_center, 2);
	CLASS_ATTR_FLOAT_ARRAY(this_class, "rgb_radial", 0, t_kinect, rgb_radial, 2);
	CLASS_ATTR_FLOAT_ARRAY(this_class, "rgb_tangential", 0, t_kinect, rgb_tangential, 2);

	CLASS_ATTR_FLOAT_ARRAY(this_class, "position", 0, t_kinect, position, 3);
	CLASS_ATTR_FLOAT_ARRAY(this_class, "orientation", 0, t_kinect, orientation, 4);

	class_register(CLASS_BOX, this_class);
}
