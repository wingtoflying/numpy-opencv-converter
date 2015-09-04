// Author: Sudeep Pillai (spillai@csail.mit.edu)
// Note: Stripped from Opencv (opencv/modules/python/src2/cv2.cpp)

# include "conversion.h"
/*
 * The following conversion functions are taken/adapted from OpenCV's cv2.cpp file
 * inside modules/python/src2 folder.
 */

static void init()
{
    import_array();
}

static int failmsg(const char *fmt, ...)
{
    char str[1000];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(str, sizeof(str), fmt, ap);
    va_end(ap);

    PyErr_SetString(PyExc_TypeError, str);
    return 0;
}

class PyAllowThreads
{
public:
    PyAllowThreads() : _state(PyEval_SaveThread()) {}
    ~PyAllowThreads()
    {
        PyEval_RestoreThread(_state);
    }
private:
    PyThreadState* _state;
};

class PyEnsureGIL
{
public:
    PyEnsureGIL() : _state(PyGILState_Ensure()) {}
    ~PyEnsureGIL()
    {
        PyGILState_Release(_state);
    }
private:
    PyGILState_STATE _state;
};

using namespace cv;
static PyObject* failmsgp(const char *fmt, ...)
{
  char str[1000];

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(str, sizeof(str), fmt, ap);
  va_end(ap);

  PyErr_SetString(PyExc_TypeError, str);
  return 0;
}

class NumpyAllocator : public MatAllocator
{
public:
    NumpyAllocator() { stdAllocator = Mat::getStdAllocator(); }
    ~NumpyAllocator() {}

    UMatData* allocate(PyObject* o, int dims, const int* sizes, int type, size_t* step) const
    {
        UMatData* u = new UMatData(this);
        u->data = u->origdata = (uchar*)PyArray_DATA((PyArrayObject*) o);
        npy_intp* _strides = PyArray_STRIDES((PyArrayObject*) o);
        for( int i = 0; i < dims - 1; i++ )
            step[i] = (size_t)_strides[i];
        step[dims-1] = CV_ELEM_SIZE(type);
        u->size = sizes[0]*step[0];
        u->userdata = o;
        return u;
    }

	UMatData* allocate(int dims0, const int* sizes, int type, void* data, size_t* step, int flags, UMatUsageFlags usageFlags) const
    {
        if( data != 0 )
        {
            CV_Error(Error::StsAssert, "The data should normally be NULL!");
            // probably this is safe to do in such extreme case
			return stdAllocator->allocate(dims0, sizes, type, data, step, flags, usageFlags);
        }
        PyEnsureGIL gil;

        int depth = CV_MAT_DEPTH(type);
        int cn = CV_MAT_CN(type);
        const int f = (int)(sizeof(size_t)/8);
        int typenum = depth == CV_8U ? NPY_UBYTE : depth == CV_8S ? NPY_BYTE :
        depth == CV_16U ? NPY_USHORT : depth == CV_16S ? NPY_SHORT :
        depth == CV_32S ? NPY_INT : depth == CV_32F ? NPY_FLOAT :
        depth == CV_64F ? NPY_DOUBLE : f*NPY_ULONGLONG + (f^1)*NPY_UINT;
        int i, dims = dims0;
        cv::AutoBuffer<npy_intp> _sizes(dims + 1);
        for( i = 0; i < dims; i++ )
            _sizes[i] = sizes[i];
        if( cn > 1 )
            _sizes[dims++] = cn;
        PyObject* o = PyArray_SimpleNew(dims, _sizes, typenum);
        if(!o)
            CV_Error_(Error::StsError, ("The numpy array of typenum=%d, ndims=%d can not be created", typenum, dims));
        return allocate(o, dims0, sizes, type, step);
    }

	bool allocate(UMatData* u, int accessFlags, UMatUsageFlags usageFlags) const
    {
		return stdAllocator->allocate(u, accessFlags, usageFlags);
    }

    void deallocate(UMatData* u) const
    {
        if(u)
        {
            PyEnsureGIL gil;
            PyObject* o = (PyObject*)u->userdata;
            Py_XDECREF(o);
            delete u;
        }
    }
 
    const MatAllocator* stdAllocator;
};
  

  
NumpyAllocator g_numpyAllocator;

NDArrayConverter::NDArrayConverter() { init(); }

void NDArrayConverter::init()
{
    import_array();
}

cv::Mat NDArrayConverter::toMat(PyObject *o)
{
	bool allowND = true;
    cv::Mat m;

    if(!o || o == Py_None)
    {
        if( !m.data )
            m.allocator = &g_numpyAllocator;
    }

	if (PyInt_Check(o))
	{
		double v[] = { static_cast<double>(PyInt_AsLong((PyObject*)o)), 0., 0., 0. };
		m = Mat(4, 1, CV_64F, v).clone();
		return m;
	}
	if (PyFloat_Check(o))
	{
		double v[] = { PyFloat_AsDouble((PyObject*)o), 0., 0., 0. };
		m = Mat(4, 1, CV_64F, v).clone();
		return m;
	}
	if (PyTuple_Check(o))
	{
		int i, sz = (int)PyTuple_Size((PyObject*)o);
		m = Mat(sz, 1, CV_64F);
		for (i = 0; i < sz; i++)
		{
			PyObject* oi = PyTuple_GET_ITEM(o, i);
			if (PyInt_Check(oi))
				m.at<double>(i) = (double)PyInt_AsLong(oi);
			else if (PyFloat_Check(oi))
				m.at<double>(i) = (double)PyFloat_AsDouble(oi);
			else
			{
				failmsg("%s is not a numerical tuple");
				m.release();
			}
		}
		return m;
	}
    if( !PyArray_Check(o) )
    {
        failmsg("toMat: Object is not a numpy array");
    }

	PyArrayObject* oarr = (PyArrayObject*)o;
	bool needcopy = false, needcast = false;
	int typenum = PyArray_TYPE(oarr), new_typenum = typenum;
    int type = typenum == NPY_UBYTE ? CV_8U : typenum == NPY_BYTE ? CV_8S :
               typenum == NPY_USHORT ? CV_16U : typenum == NPY_SHORT ? CV_16S :
               typenum == NPY_INT || typenum == NPY_LONG ? CV_32S :
               typenum == NPY_FLOAT ? CV_32F :
               typenum == NPY_DOUBLE ? CV_64F : -1;

    if( type < 0 )
	{
		if (typenum == NPY_INT64 || typenum == NPY_UINT64 || type == NPY_LONG)
		{
			needcopy = needcast = true;
			new_typenum = NPY_INT;
			type = CV_32S;
		}
		else
			failmsg("toMat: Data type = %d is not supported", typenum);
    }

    int ndims = PyArray_NDIM(o);
	//std::cerr << "Diminsion : " << ndims << std::endl;
    if(ndims >= CV_MAX_DIM)
    {
        failmsg("toMat: Dimensionality (=%d) is too high", ndims);
    }

    int size[CV_MAX_DIM+1];
    size_t step[CV_MAX_DIM+1], elemsize = CV_ELEM_SIZE1(type);
    const npy_intp* _sizes = PyArray_DIMS(o);
	const npy_intp* _strides = PyArray_STRIDES(o);
	bool ismultichannel = ndims == 3 && _sizes[2] <= CV_CN_MAX;
    bool transposed = false;
	for( int i = ndims-1; i >= 0 && !needcopy; i-- )
	{
		// these checks handle cases of
		//  a) multi-dimensional (ndims > 2) arrays, as well as simpler 1- and 2-dimensional cases
		//  b) transposed arrays, where _strides[] elements go in non-descending order
		//  c) flipped arrays, where some of _strides[] elements are negative
		if( (i == ndims-1 && (size_t)_strides[i] != elemsize) ||
			(i < ndims-1 && _strides[i] < _strides[i+1]) )
			needcopy = true;
	}

	if( ismultichannel && _strides[1] != (npy_intp)elemsize*_sizes[2] )
		needcopy = true;

	if (needcopy)
	{
		//if (info.outputarg)
		//{
		//	failmsg("Layout of the output array %s is incompatible with cv::Mat (step[ndims-1] != elemsize or step[1] != elemsize*nchannels)", info.name);
		//	//return false;
		//}

		if( needcast ) {
			o = PyArray_Cast(oarr, new_typenum);
			oarr = (PyArrayObject*) o;
		}
		else {
			oarr = PyArray_GETCONTIGUOUS(oarr);
			o = (PyObject*)oarr;
		}

		_strides = PyArray_STRIDES(oarr);
	}

	for (int i = 0; i < ndims; i++)
	{
		size[i] = (int)_sizes[i];
		step[i] = (size_t)_strides[i];
	}

	// handle degenerate case
	if (ndims == 0) {
		size[ndims] = 1;
		step[ndims] = elemsize;
		ndims++;
	}

	if (ismultichannel)
	{
		ndims--;
		type |= CV_MAKETYPE(0, size[2]);
	}

    
	if (ndims > 2 && !allowND)
    {
        failmsg("toMat: Object has more than 2 dimensions");
    }
    
    m = Mat(ndims, size, type, PyArray_DATA(o), step);
    // m.u = g_numpyAllocator.allocate(o, ndims, size, type, step);
    
	if (m.data)
	{
		m.u = g_numpyAllocator.allocate(o, ndims, size, type, step);
		m.addref();
		Py_INCREF(o);
	};
	m.allocator = &g_numpyAllocator;

    if( transposed )
    {
        Mat tmp;
        tmp.allocator = &g_numpyAllocator;
        transpose(m, tmp);
        m = tmp;
    }
    return m;
}

PyObject* NDArrayConverter::toNDArray(const cv::Mat& m)
{
  if( !m.data )
        Py_RETURN_NONE;
    Mat temp, *p = (Mat*)&m;
    if(!p->u || p->allocator != &g_numpyAllocator)
    {
        temp.allocator = &g_numpyAllocator;
        m.copyTo(temp);
        p = &temp;
    }
    PyObject* o = (PyObject*)p->u->userdata;
    Py_INCREF(o);
    // p->addref();
    // pyObjectFromRefcount(p->refcount);
    return o; 

}
