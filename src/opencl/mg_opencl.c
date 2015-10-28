#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>

#include "mg_idl_export.h"

#if defined(__APPLE__) && defined(__MACH__)
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include "mg_hashtable.h"

// CL_INIT macro needs to know IDL_cl_init definition
static void IDL_cl_init(int argc, IDL_VPTR *argv, char *argk);


// CL_VARIABLE flags
#define CL_V_VIEW 128

typedef struct {
  UCHAR type;
  UCHAR flags;
  IDL_MEMINT n_elts;
  UCHAR n_dim;
  IDL_ARRAY_DIM dim;
  cl_mem buffer;
} CL_VARIABLE;
typedef CL_VARIABLE *CL_VPTR;

typedef struct {
  UCHAR simple;
  char *expr;
  cl_kernel kernel;
} CL_KERNEL;


static cl_context current_context      = NULL;
static cl_command_queue current_queue  = NULL;
static cl_platform_id current_platform = NULL;
static cl_device_id current_device     = NULL;

MG_Table kernel_table;


IDL_MSG_BLOCK msg_block;

static IDL_MSG_DEF msg_arr[] = {
#define OPENCL_ERROR 0
  { "OPENCL_ERROR",                  "%NError: %s." },
#define OPENCL_NO_PLATFORMS -1
  { "OPENCL_NO_PLATFORMS",           "%NNo valid platforms found." },
#define OPENCL_NO_DEVICES -2
  { "OPENCL_NO_DEVICES",             "%NNo valid devices found." },
#define OPENCL_MISSING_KEYWORD -3
  { "OPENCL_MISSING_KEYWORD",        "%NMissing required keyword: %s." },
#define OPENCL_INCORRECT_N_PARAMS -4
  { "OPENCL_INCORRECT_N_PARAMS",     "%NIncorrect number of parameters." },
#define OPENCL_INCORRECT_PARAM_TYPE -5
  { "OPENCL_INCORRECT_PARAM_TYPE",   "%NIncorrect parameter type." },
#define OPENCL_INVALID_PLATFORM_INDEX -6
  { "OPENCL_INVALID_PLATFORM_INDEX", "%NInvalid platform index: %d." },
};


void mg_cl_release_kernel(void *k) {
  cl_kernel kernel = (cl_kernel) k;
  cl_program program;
  cl_int err = 0;

  if (kernel == NULL) return;

  // get program from the kernel
  err = clGetKernelInfo(kernel, CL_KERNEL_PROGRAM, sizeof(cl_program), &program, NULL);
  if (err < 0 || program == NULL) return;

  err = clReleaseKernel(kernel);
  if (err < 0) return;

  err = clReleaseProgram(program);
}


#pragma mark --- helper macros ---

#define STRINGIFY(text) #text

#define CL_INIT                   \
  if (!current_context) {         \
    IDL_cl_init(0, NULL, NULL);   \
  }

#define CL_SET_ERROR(err)                      \
  if (kw.error_present) {                      \
    kw.error->type = IDL_TYP_LONG;             \
    kw.error->value.l = err;                   \
  }


#pragma mark --- query routines ---

static IDL_VPTR IDL_cl_platforms(int argc, IDL_VPTR *argv, char *argk) {
  int nargs;

  cl_int err = 0;

  cl_platform_id *platform_ids;
  cl_uint num_platforms;
  int p;

  char *info_data;
  size_t info_size;

  IDL_MEMINT nplatforms = 1;
  void *idl_platforms_data;
  IDL_VPTR platform_result;

  static IDL_STRUCT_TAG_DEF platform_tags[] = {
    {"NAME",       0, (void *) IDL_TYP_STRING},
    {"VENDOR",     0, (void *) IDL_TYP_STRING},
    {"VERSION",    0, (void *) IDL_TYP_STRING},
    {"PROFILE",    0, (void *) IDL_TYP_STRING},
    {"EXTENSIONS", 0, (void *) IDL_TYP_STRING},
    { 0 }
  };

  typedef struct platform {
    IDL_STRING name;
    IDL_STRING vendor;
    IDL_STRING version;
    IDL_STRING profile;
    IDL_STRING extensions;
  } Platform;

  Platform *platform_data;

  typedef struct {
    IDL_KW_RESULT_FIRST_FIELD;
    IDL_VPTR count;
    int count_present;
    IDL_VPTR error;
    int error_present;
  } KW_RESULT;

  static IDL_KW_PAR kw_pars[] = {
    { "COUNT", IDL_TYP_LONG, 1, IDL_KW_OUT,
      IDL_KW_OFFSETOF(count_present), IDL_KW_OFFSETOF(count) },
    { "ERROR", IDL_TYP_LONG, 1, IDL_KW_OUT,
      IDL_KW_OFFSETOF(error_present), IDL_KW_OFFSETOF(error) },
    { NULL }
  };

  KW_RESULT kw;

  nargs = IDL_KWProcessByOffset(argc, argv, argk, kw_pars, (IDL_VPTR *) NULL, 1, &kw);

  // initialize error
  CL_SET_ERROR(err);

  // query for number of platforms
  err = clGetPlatformIDs(0, NULL, &num_platforms);
  if (err < 0) {
    CL_SET_ERROR(err);
    IDL_KW_FREE;
    return IDL_GettmpLong(err);
  }

  nplatforms = num_platforms;

  if (kw.count_present) {
    kw.count->type = IDL_TYP_LONG;
    kw.count->value.l = nplatforms;
  }

  platform_data = (Platform *) calloc(num_platforms, sizeof(Platform));

  platform_ids = (cl_platform_id *) calloc(num_platforms,
                                           sizeof(cl_platform_id));
  clGetPlatformIDs(num_platforms, platform_ids, NULL);

  for (p = 0; p < num_platforms; p++) {
#define GET_PLATFORM_STR_PROP(PROP, FIELD)                        \
    err = clGetPlatformInfo(platform_ids[p], PROP,                \
                            0, NULL, &info_size);                 \
    info_data = (char *) malloc(info_size);                       \
    err = clGetPlatformInfo(platform_ids[p], PROP,                \
                            info_size, info_data, NULL);          \
    IDL_StrStore(&platform_data[p].FIELD, info_data);             \
    free(info_data);                                              \

    GET_PLATFORM_STR_PROP(CL_PLATFORM_NAME, name)
    GET_PLATFORM_STR_PROP(CL_PLATFORM_VENDOR, vendor)
    GET_PLATFORM_STR_PROP(CL_PLATFORM_VERSION, version)
    GET_PLATFORM_STR_PROP(CL_PLATFORM_PROFILE, profile)
    GET_PLATFORM_STR_PROP(CL_PLATFORM_EXTENSIONS, extensions)
  }

  IDL_KW_FREE;

  free(platform_ids);

  idl_platforms_data = IDL_MakeStruct("CL_PLATFORM", platform_tags);

  platform_result = IDL_ImportArray(1,
                                    &nplatforms,
                                    IDL_TYP_STRUCT,
                                    (UCHAR *) platform_data,
                                    0,
                                    idl_platforms_data);

  return(platform_result);
}


static IDL_VPTR IDL_cl_devices(int argc, IDL_VPTR *argv, char *argk) {
  int nargs;

  cl_int err = 0;

  cl_platform_id *platform_ids;
  cl_uint num_platforms;
  int platform_index;

  cl_device_id *device_ids;
  cl_uint num_devices;
  int d = 0;

  char *info_data;
  size_t info_size;

  IDL_MEMINT ndevices = 1;
  void *idl_devices_data;
  IDL_VPTR device_result;

  cl_ulong ulong_info;
  cl_uint uint_info;
  cl_bool bool_info;

  static IDL_STRUCT_TAG_DEF device_tags[] = {
    { "NAME",                      0, (void *) IDL_TYP_STRING  },
    { "VENDOR",                    0, (void *) IDL_TYP_STRING  },
    { "VENDOR_ID",                 0, (void *) IDL_TYP_ULONG   },
    { "TYPE",                      0, (void *) IDL_TYP_STRING  },
    { "EXTENSIONS",                0, (void *) IDL_TYP_STRING  },
    { "PROFILE",                   0, (void *) IDL_TYP_STRING  },
    { "GLOBAL_MEM_SIZE",           0, (void *) IDL_TYP_ULONG64 },
    { "GLOBAL_MEM_CACHE_SIZE",     0, (void *) IDL_TYP_ULONG64 },
    { "ADDRESS_BITS",              0, (void *) IDL_TYP_ULONG   },
    { "AVAILABLE",                 0, (void *) IDL_TYP_BYTE    },
    { "COMPILER_AVAILABLE",        0, (void *) IDL_TYP_BYTE    },
    { "ENDIAN_LITTLE",             0, (void *) IDL_TYP_BYTE    },
    { "ERROR_CORRECTION_SUPPORT",  0, (void *) IDL_TYP_BYTE    },
    { "DEVICE_VERSION",            0, (void *) IDL_TYP_STRING  },
    { "DRIVER_VERSION",            0, (void *) IDL_TYP_STRING  },
    { 0 }
  };

  typedef struct device {
    IDL_STRING name;
    IDL_STRING vendor;
    IDL_ULONG vendor_id;
    IDL_STRING type;
    IDL_STRING extensions;
    IDL_STRING profile;
    IDL_ULONG64 global_mem_size;
    IDL_ULONG64 global_mem_cache_size;
    IDL_ULONG address_bits;
    UCHAR available;
    UCHAR compiler_available;
    UCHAR endian_little;
    UCHAR error_correction_support;
    IDL_STRING device_version;
    IDL_STRING driver_version;
  } Device;

  Device *device_data;

  typedef struct {
    IDL_KW_RESULT_FIRST_FIELD;
    IDL_VPTR count;
    int count_present;
    IDL_LONG current;
    IDL_VPTR error;
    int error_present;
    IDL_LONG gpu;
    IDL_VPTR platform;
    int platform_present;
  } KW_RESULT;

  static IDL_KW_PAR kw_pars[] = {
    { "COUNT", IDL_TYP_LONG, 1, IDL_KW_OUT,
      IDL_KW_OFFSETOF(count_present), IDL_KW_OFFSETOF(count) },
    { "CURRENT", IDL_TYP_LONG, 1, IDL_KW_ZERO,
      0, IDL_KW_OFFSETOF(current) },
    { "ERROR", IDL_TYP_LONG, 1, IDL_KW_OUT,
      IDL_KW_OFFSETOF(error_present), IDL_KW_OFFSETOF(error) },
    { "GPU", IDL_TYP_LONG, 1, IDL_KW_ZERO,
      0, IDL_KW_OFFSETOF(gpu) },
    { "PLATFORM", IDL_TYP_UNDEF, 1, IDL_KW_VIN,
      IDL_KW_OFFSETOF(platform_present), IDL_KW_OFFSETOF(platform) },
    { NULL }
  };

  KW_RESULT kw;

  nargs = IDL_KWProcessByOffset(argc, argv, argk, kw_pars, (IDL_VPTR *) NULL, 1, &kw);

  // initialize error
  CL_SET_ERROR(err);

  if (kw.count_present) {
    kw.count->type = IDL_TYP_LONG;
    kw.count->value.l = 0;
  }

  if (kw.current && !current_device) {
    IDL_KW_FREE;

    return(IDL_GettmpLong(-1));
  }

  // query for number of platforms
  err = clGetPlatformIDs(0, NULL, &num_platforms);
  if (err < 0) {
    CL_SET_ERROR(err);
    IDL_KW_FREE;
    return IDL_GettmpLong(err);
  }

  if (num_platforms == 0) {
    IDL_KW_FREE;

    IDL_MessageFromBlock(msg_block, OPENCL_NO_PLATFORMS, IDL_MSG_RET);
    return(IDL_GettmpLong(-1));
  }

  if (!kw.platform_present) {
    platform_index = 0;
  } else {
    platform_index = kw.platform->value.l;
  }

  if (platform_index >= num_platforms) {
    IDL_KW_FREE;

    IDL_MessageFromBlock(msg_block, OPENCL_INVALID_PLATFORM_INDEX, IDL_MSG_RET, platform_index);
    return(IDL_GettmpLong(-1));
  }

  platform_ids = (cl_platform_id *) calloc(num_platforms,
                                           sizeof(cl_platform_id));
  err = clGetPlatformIDs(num_platforms, platform_ids, NULL);
  if (err < 0) {
    CL_SET_ERROR(err);
    IDL_KW_FREE;
    return IDL_GettmpLong(err);
  }

  err = clGetDeviceIDs(platform_ids[platform_index],
                       kw.gpu ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_ALL,
                       0, NULL, &num_devices);
  if (err < 0) {
    CL_SET_ERROR(err);
    IDL_KW_FREE;
    return IDL_GettmpLong(err);
  }

  if (kw.current) num_devices = 1;
  ndevices = num_devices;

  if (kw.count_present) {
    kw.count->type = IDL_TYP_LONG;
    kw.count->value.l = ndevices;
  }

  if (num_devices == 0) {
    return(IDL_GettmpLong(-1));
  }

  device_data = (Device *) calloc(num_devices, sizeof(Device));

  device_ids = (cl_device_id *) calloc(num_devices, sizeof(cl_device_id));
  clGetDeviceIDs(platform_ids[platform_index],
                 kw.gpu ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_ALL,
                 num_devices, device_ids, NULL);

  if (kw.current) {
#define GET_DEVICE_STR_PROP(DEVICE, PROP, FIELD)          \
    err = clGetDeviceInfo(DEVICE, PROP,                   \
                          0, NULL, &info_size);           \
    info_data = (char *) malloc(info_size);               \
    err = clGetDeviceInfo(DEVICE, PROP,                   \
                          info_size, info_data, NULL);    \
    IDL_StrStore(&device_data[d].FIELD, info_data);       \
    free(info_data);

#define GET_DEVICE_PROP(DEVICE, PROP, FIELD, VAR)         \
    err = clGetDeviceInfo(DEVICE, PROP,                   \
                          sizeof(VAR), &VAR, NULL);       \
    device_data[d].FIELD = VAR;

    GET_DEVICE_STR_PROP(current_device, CL_DEVICE_NAME, name)
    GET_DEVICE_STR_PROP(current_device, CL_DEVICE_VENDOR, vendor)
    GET_DEVICE_STR_PROP(current_device, CL_DEVICE_EXTENSIONS, extensions)
    GET_DEVICE_STR_PROP(current_device, CL_DEVICE_PROFILE, profile)

    GET_DEVICE_PROP(current_device, CL_DEVICE_VENDOR_ID, vendor_id, uint_info)
    err = clGetDeviceInfo(current_device,
                          CL_DEVICE_VENDOR_ID,
                          sizeof(uint_info),
                          &uint_info, NULL);
    if (uint_info == CL_DEVICE_TYPE_CPU) {
      IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_CPU");
    } else if (uint_info == CL_DEVICE_TYPE_GPU) {
      IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_GPU");
    } else if (uint_info == CL_DEVICE_TYPE_GPU) {
      IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_ACCELERATOR");
    } else if (uint_info == CL_DEVICE_TYPE_GPU) {
      IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_DEFAULT");
    } else if (uint_info == CL_DEVICE_TYPE_ALL) {
      IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_ALL");
    }

    GET_DEVICE_PROP(current_device, CL_DEVICE_GLOBAL_MEM_SIZE, global_mem_size, ulong_info)
    GET_DEVICE_PROP(current_device, CL_DEVICE_GLOBAL_MEM_CACHE_SIZE, global_mem_cache_size, ulong_info)
    GET_DEVICE_PROP(current_device, CL_DEVICE_ADDRESS_BITS, address_bits, uint_info)
    GET_DEVICE_PROP(current_device, CL_DEVICE_AVAILABLE, available, bool_info)
    GET_DEVICE_PROP(current_device, CL_DEVICE_COMPILER_AVAILABLE, compiler_available, bool_info)

    GET_DEVICE_PROP(current_device, CL_DEVICE_ENDIAN_LITTLE, endian_little, bool_info)
    GET_DEVICE_PROP(current_device, CL_DEVICE_ERROR_CORRECTION_SUPPORT, error_correction_support, bool_info)

    GET_DEVICE_STR_PROP(current_device, CL_DEVICE_VERSION, device_version)
    GET_DEVICE_STR_PROP(current_device, CL_DRIVER_VERSION, driver_version)
  } else {
    for (d = 0; d < num_devices; d++) {
      GET_DEVICE_STR_PROP(device_ids[d], CL_DEVICE_NAME, name)
      GET_DEVICE_STR_PROP(device_ids[d], CL_DEVICE_VENDOR, vendor)
      GET_DEVICE_STR_PROP(device_ids[d], CL_DEVICE_EXTENSIONS, extensions)
      GET_DEVICE_STR_PROP(device_ids[d], CL_DEVICE_PROFILE, profile)

      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_VENDOR_ID, vendor_id, uint_info)
      err = clGetDeviceInfo(device_ids[d],
                            CL_DEVICE_VENDOR_ID,
                            sizeof(uint_info),
                            &uint_info, NULL);
      if (uint_info == CL_DEVICE_TYPE_CPU) {
        IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_CPU");
      } else if (uint_info == CL_DEVICE_TYPE_GPU) {
        IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_GPU");
      } else if (uint_info == CL_DEVICE_TYPE_GPU) {
        IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_ACCELERATOR");
      } else if (uint_info == CL_DEVICE_TYPE_GPU) {
        IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_DEFAULT");
      } else if (uint_info == CL_DEVICE_TYPE_ALL) {
        IDL_StrStore(&device_data[d].type, "CL_DEVICE_TYPE_ALL");
      }

      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_GLOBAL_MEM_SIZE, global_mem_size, ulong_info)
      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_GLOBAL_MEM_CACHE_SIZE, global_mem_cache_size, ulong_info)
      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_ADDRESS_BITS, address_bits, uint_info)
      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_AVAILABLE, available, bool_info)
      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_COMPILER_AVAILABLE, compiler_available, bool_info)

      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_ENDIAN_LITTLE, endian_little, bool_info)
      GET_DEVICE_PROP(device_ids[d], CL_DEVICE_ERROR_CORRECTION_SUPPORT, error_correction_support, bool_info)

      GET_DEVICE_STR_PROP(device_ids[d], CL_DEVICE_VERSION, device_version)
      GET_DEVICE_STR_PROP(device_ids[d], CL_DRIVER_VERSION, driver_version)

      // TODO: there are more properties to check, * add soon
      /*
        CL_DEVICE_DOUBLE_FP_CONFIG,
        CL_DEVICE_EXECUTION_CAPABILITIES,
        CL_DEVICE_GLOBAL_MEM_CACHE_TYPE,
        CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE,
        CL_DEVICE_HALF_FP_CONFIG,
        CL_DEVICE_IMAGE_SUPPORT,
        CL_DEVICE_IMAGE2D_MAX_HEIGHT,
        CL_DEVICE_IMAGE2D_MAX_WIDTH,
        CL_DEVICE_IMAGE3D_MAX_DEPTH,
        CL_DEVICE_IMAGE3D_MAX_HEIGHT,
        CL_DEVICE_IMAGE3D_MAX_WIDTH,
        CL_DEVICE_LOCAL_MEM_SIZE,
        CL_DEVICE_LOCAL_MEM_TYPE,
        CL_DEVICE_MAX_CLOCK_FREQUENCY, *
        CL_DEVICE_MAX_COMPUTE_UNITS, *
        CL_DEVICE_MAX_CONSTANT_ARGS, *
        CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE, *
        CL_DEVICE_MAX_MEM_ALLOC_SIZE, *
        CL_DEVICE_MAX_PARAMETER_SIZE, *
        CL_DEVICE_MAX_READ_IMAGE_ARGS,
        CL_DEVICE_MAX_SAMPLERS,
        CL_DEVICE_MAX_WORK_GROUP_SIZE, *
        CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS, *
        CL_DEVICE_MAX_WORK_ITEM_SIZES, *
        CL_DEVICE_MAX_WRITE_IMAGE_ARGS,
        CL_DEVICE_MEM_BASE_ADDR_ALIGN,
        CL_DEVICE_MIN_DATA_TYPE_ALIGN_SIZE,
        CL_DEVICE_PREFERRED_VECTOR_WIDTH_CHAR,
        CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT,
        CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT,
        CL_DEVICE_PREFERRED_VECTOR_WIDTH_LONG,
        CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT,
        CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE,
        CL_DEVICE_PROFILING_TIMER_RESOLUTION,
        CL_DEVICE_QUEUE_PROPERTIES,
        CL_DEVICE_SINGLE_FP_CONFIG
      */
    }
  }

  IDL_KW_FREE;

  free(device_ids);

  idl_devices_data = IDL_MakeStruct("CL_DEVICE", device_tags);

  device_result = IDL_ImportArray(1,
                                  &ndevices,
                                  IDL_TYP_STRUCT,
                                  (UCHAR *) device_data,
                                  0,
                                  idl_devices_data);

  return(device_result);
}


static void IDL_cl_help(int argc, IDL_VPTR *argv, char *argk) {
  int nargs, d;
  cl_int err = 0;

  CL_VPTR cl_var;
  CL_KERNEL *kernel;
  char *varname = IDL_VarName(argv[0]);

  typedef struct {
    IDL_KW_RESULT_FIRST_FIELD;
    IDL_VPTR error;
    int error_present;
    IDL_LONG kernel;
  } KW_RESULT;

  static IDL_KW_PAR kw_pars[] = {
    { "ERROR", IDL_TYP_LONG, 1, IDL_KW_OUT,
      IDL_KW_OFFSETOF(error_present), IDL_KW_OFFSETOF(error) },
    { "KERNEL", IDL_TYP_LONG, 1, IDL_KW_ZERO,
      0, IDL_KW_OFFSETOF(kernel) },
    { NULL }
  };

  KW_RESULT kw;

  nargs = IDL_KWProcessByOffset(argc, argv, argk, kw_pars, (IDL_VPTR *) NULL, 1, &kw);

  // initialize error
  CL_SET_ERROR(err);

  CL_INIT;

  if (nargs == 0) {
    size_t info_size;
    char *info_data;

    err = clGetDeviceInfo(current_device, CL_DEVICE_NAME, 0, NULL, &info_size);
    info_data = (char *) malloc(info_size);
    err = clGetDeviceInfo(current_device, CL_DEVICE_NAME, info_size, info_data, NULL);
    printf("Current device: %s\n", info_data);
    free(info_data);
  } else {
    if (kw.kernel) {
      kernel = (CL_KERNEL *) argv[0]->value.ptrint;
      printf("%-16.16s%-9.9s = '%s'\n", varname[0] == '<' ? "<Expression>" : varname, "CL_KERNEL", kernel->expr);
    } else {
      cl_var = (CL_VPTR) argv[0]->value.ptrint;
      printf("%-16.16sCL_%-6.6s = Array[", varname[0] == '<' ? "<Expression>" : varname, IDL_TypeName[cl_var->type]);
      for (d = 0; d < cl_var->n_dim; d++) {
        printf("%s%lld", d == 0 ? "" : ", ", cl_var->dim[d]);
      }
      printf("]\n");
    }
  }

  IDL_KW_FREE;
}


#pragma mark --- initialization ---

static void IDL_cl_init(int argc, IDL_VPTR *argv, char *argk) {
  int nargs;
  cl_int err = 0;

  cl_platform_id *platform_ids;
  cl_uint num_platforms;
  int p, platform_index = 0;

  cl_device_id *device_ids;
  cl_uint num_devices;
  int device_index = 0;

  typedef struct {
    IDL_KW_RESULT_FIRST_FIELD;
    IDL_VPTR device;
    int device_present;
    IDL_VPTR error;
    int error_present;
    IDL_LONG gpu;
    IDL_VPTR platform;
    int platform_present;
  } KW_RESULT;

  static IDL_KW_PAR kw_pars[] = {
    { "DEVICE", IDL_TYP_UNDEF, 1, IDL_KW_VIN,
      IDL_KW_OFFSETOF(device_present), IDL_KW_OFFSETOF(device) },
    { "ERROR", IDL_TYP_LONG, 1, IDL_KW_OUT,
      IDL_KW_OFFSETOF(error_present), IDL_KW_OFFSETOF(error) },
    { "GPU", IDL_TYP_LONG, 1, IDL_KW_ZERO,
      0, IDL_KW_OFFSETOF(gpu) },
    { "PLATFORM", IDL_TYP_UNDEF, 1, IDL_KW_VIN,
      IDL_KW_OFFSETOF(platform_present), IDL_KW_OFFSETOF(platform) },
    { NULL }
  };

  KW_RESULT kw;

  nargs = IDL_KWProcessByOffset(argc, argv, argk, kw_pars, (IDL_VPTR *) NULL, 1, &kw);

  // initialize error
  CL_SET_ERROR(err);

  if (kw.platform_present) {
    platform_index = kw.platform->value.l;
  } else {
    char *platform_index_env;
    platform_index_env = getenv("CL_DEFAULT_PLATFORM");
    if (platform_index_env != NULL) {
      platform_index = atoi(platform_index_env);
    }
  }
  if (kw.device_present) {
    device_index = kw.device->value.l;
  } else {
    char *device_index_env;
    device_index_env = getenv("CL_DEFAULT_DEVICE");
    if (device_index_env != NULL) {
      device_index = atoi(device_index_env);
    }
  }

  // query for number of platforms
  err = clGetPlatformIDs(0, NULL, &num_platforms);
  if (err < 0) {
    CL_SET_ERROR(err);
    IDL_KW_FREE;
    return;
  }

  if (num_platforms == 0) {
    IDL_KW_FREE;
    IDL_MessageFromBlock(msg_block, OPENCL_NO_PLATFORMS, IDL_MSG_RET);
    CL_SET_ERROR(-1);
    return;
  }

  platform_ids = (cl_platform_id *) calloc(num_platforms,
                                           sizeof(cl_platform_id));
  clGetPlatformIDs(num_platforms, platform_ids, NULL);

  // if a platform is specified and looking for a GPU, search all platforms
  // for first available GPU
  if (!kw.platform_present && kw.gpu) {
    for (p = 0; p < num_platforms; p++) {
      err = clGetDeviceIDs(platform_ids[p],
                           CL_DEVICE_TYPE_GPU,
                           0, NULL, &num_devices);

      // num_devices only valid if err is 0
      if (err == 0 && num_devices > 0) {
        platform_index = p;
        break;
      }
    }
  } else {
    err = clGetDeviceIDs(platform_ids[platform_index],
                         kw.gpu ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_ALL,
                         0, NULL, &num_devices);
    if (err < 0) {
      CL_SET_ERROR(err);
      IDL_KW_FREE;
      return;
    }
  }

  // failed to find any devices
  if (err != 0 || num_devices == 0) {
    free(platform_ids);
    IDL_KW_FREE;
    IDL_MessageFromBlock(msg_block, OPENCL_NO_DEVICES, IDL_MSG_RET);
    CL_SET_ERROR(-1);
    return;
  }

  device_ids = (cl_device_id *) calloc(num_devices, sizeof(cl_device_id));
  err = clGetDeviceIDs(platform_ids[platform_index],
                       kw.gpu ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_ALL,
                       num_devices, device_ids, NULL);
  if (err < 0) {
    free(platform_ids);
    IDL_KW_FREE;
    IDL_MessageFromBlock(msg_block, OPENCL_NO_DEVICES, IDL_MSG_RET);
    CL_SET_ERROR(-1);
    return;
  }

  if (current_queue != NULL) {
    clReleaseCommandQueue(current_queue);
    clReleaseContext(current_context);

    // kernels in table are associated with old context
    mg_table_free(&kernel_table, mg_cl_release_kernel);
    kernel_table = mg_table_new(0);
  }

  current_platform = platform_ids[platform_index];
  current_device = device_ids[device_index];
  current_context = clCreateContext(NULL, 1, &current_device,
                                    NULL, NULL, &err);
  if (err < 0) {
    CL_SET_ERROR(err);
    IDL_KW_FREE;
    return;
  }
  current_queue = clCreateCommandQueue(current_context, current_device, 0, &err);
  if (err < 0) {
    CL_SET_ERROR(err);
    IDL_KW_FREE;
    return;
  }

  free(platform_ids);
  free(device_ids);

  IDL_KW_FREE;
}


#pragma mark --- memory ---

static void IDL_cl_free(int argc, IDL_VPTR *argv, char *argk) {
  int nargs;
  cl_int err = 0;

  typedef struct {
    IDL_KW_RESULT_FIRST_FIELD;
    IDL_VPTR error;
    int error_present;
  } KW_RESULT;

  static IDL_KW_PAR kw_pars[] = {
    { "ERROR", IDL_TYP_LONG, 1, IDL_KW_OUT,
      IDL_KW_OFFSETOF(error_present), IDL_KW_OFFSETOF(error) },
    { NULL }
  };

  KW_RESULT kw;

  nargs = IDL_KWProcessByOffset(argc, argv, argk, kw_pars, (IDL_VPTR *) NULL, 1, &kw);

  // initialize error
  CL_SET_ERROR(err);

  CL_INIT;

  if (argv[0]->flags & IDL_V_ARR) {
    int v;
    CL_VPTR *cl_var_arr = (CL_VPTR *) argv[0]->value.arr->data;
    for (v = 0; v < argv[0]->value.arr->n_elts; v++) {
      cl_mem buffer = (cl_mem) cl_var_arr[v]->buffer;
      if (cl_var_arr[v]->flags & CL_V_VIEW) {
        err = 0;
      } else {
        err = clReleaseMemObject(buffer);
      }
      cl_var_arr[v]->type = IDL_TYP_UNDEF;
      cl_var_arr[v]->n_dim = 0;
      cl_var_arr[v]->n_elts = 0;
      free(cl_var_arr[v]);
    }
  } else {
    CL_VPTR cl_var = (CL_VPTR) argv[0]->value.ptrint;
    cl_mem buffer = (cl_mem) cl_var->buffer;

    if (cl_var->flags & CL_V_VIEW) {
      err = 0;
    } else {
      err = clReleaseMemObject(buffer);
    }
    cl_var->type = IDL_TYP_UNDEF;
    cl_var->n_dim = 0;
    cl_var->n_elts = 0;
    free(cl_var);
  }

  CL_SET_ERROR(err);

  IDL_KW_FREE;
}


#pragma mark --- Lifecycle ---


// handle any cleanup required
static void mg_cl_exit_handler(void) {
  mg_table_free(&kernel_table, mg_cl_release_kernel);

  clReleaseCommandQueue(current_queue);
  clReleaseContext(current_context);
}


int IDL_Load(void) {
  static IDL_SYSFUN_DEF2 function_addr[] = {
    { IDL_cl_platforms, "MG_CL_PLATFORMS", 0, 0, IDL_SYSFUN_DEF_F_KEYWORDS, 0 },
    { IDL_cl_devices,   "MG_CL_DEVICES",   0, 0, IDL_SYSFUN_DEF_F_KEYWORDS, 0 },
  };

  static IDL_SYSFUN_DEF2 procedure_addr[] = {
    { (IDL_SYSRTN_GENERIC) IDL_cl_init, "MG_CL_INIT", 0, 0, IDL_SYSFUN_DEF_F_KEYWORDS, 0 },
    { (IDL_SYSRTN_GENERIC) IDL_cl_help, "MG_CL_HELP", 0, 1, IDL_SYSFUN_DEF_F_KEYWORDS, 0 },
    { (IDL_SYSRTN_GENERIC) IDL_cl_free, "MG_CL_FREE", 1, 1, IDL_SYSFUN_DEF_F_KEYWORDS, 0 },
  };

  if (!(msg_block = IDL_MessageDefineBlock("opencl", IDL_CARRAY_ELTS(msg_arr), msg_arr))) {
    return(IDL_FALSE);
  }

  IDL_ExitRegister(mg_cl_exit_handler);

  kernel_table = mg_table_new(0);

  // default initialization
  IDL_cl_init(0, NULL, NULL);

  return IDL_SysRtnAdd(procedure_addr, FALSE, IDL_CARRAY_ELTS(procedure_addr))
         && IDL_SysRtnAdd(function_addr, TRUE, IDL_CARRAY_ELTS(function_addr));
}