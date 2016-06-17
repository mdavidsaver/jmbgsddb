
#include <list>
#include <sstream>

#include "flame/base.h"

#include "pyflame.h"

#define NO_IMPORT_ARRAY
#define PY_ARRAY_UNIQUE_SYMBOL FLAME_PyArray_API
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/ndarrayobject.h>

/** Translate python dict to Config
 *
 *  {}      -> Config
 *    float   -> double
 *    str     -> string
 *    [{}]    -> vector<Config>  (recurse)
 *    ndarray -> vector<double>
 *    TODO: [0.0]   -> vector<double>
 */
void Dict2Config(Config& ret, PyObject *dict, unsigned depth)
{
    if(depth>3)
        throw std::runtime_error("too deep for Dict2Config");

    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while(PyDict_Next(dict, &pos, &key, &value)) {
        PyRef<> keyref(key, borrow());
        PyCString skey(keyref);
        const char *kname = skey.c_str();

        PyTypeObject *valuetype = (PyTypeObject*)PyObject_Type(value);
        if(valuetype==&PyFloat_Type) { // scalar double
            double val = PyFloat_AsDouble(value);
            ret.set<double>(kname, val);

        } else if(valuetype==&PyInt_Type) { // scalar integer (treated as double)
            long val = PyInt_AsLong(value);
            ret.set<double>(kname, val);

        } else if(PyString_Check(value)) { // string
            PyRef<> valref(value, borrow());
            PyCString sval(valref);
            const char *val = sval.c_str();

            ret.set<std::string>(kname, val);

        } else if(PyArray_Check(value)) { // array (ndarray)
            PyRef<> arr(PyArray_ContiguousFromAny(value, NPY_DOUBLE, 0, 2));
            double *buf = (double*)PyArray_DATA(arr.as<PyArrayObject>());
            std::vector<double> temp(PyArray_SIZE(arr.as<PyArrayObject>()));
            std::copy(buf, buf+temp.size(), temp.begin());

            ret.swap<std::vector<double> >(kname, temp);

        } else if(PySequence_Check(value)) { // list of dict
            Py_ssize_t N = PySequence_Size(value);

            Config::vector_t output;
            output.reserve(N);

            for(Py_ssize_t i=0; i<N; i++) {
                PyObject *elem = PySequence_GetItem(value, i);
                assert(elem);

                if(!PyDict_Check(elem))
                    throw std::invalid_argument("lists must contain only dict()s");

                //output.push_back(ret.new_scope()); // TODO: can't use scoping here since iteration order of PyDict_Next() is not stable
                output.push_back(Config());

                Dict2Config(output.back(), elem, depth+1); // inheirt parent scope
            }

            ret.set<Config::vector_t>(kname, output);

        } else {
            std::ostringstream msg;
            msg<<"Must be a dict, not "<<valuetype->tp_name;
            throw std::invalid_argument(msg.str());
        }
    }
}

namespace {

PyObject* pydirname(PyObject *obj)
{
    if(!obj) return NULL;

    PyRef<> ospath(PyImport_ImportModule("os.path"));
    return PyObject_CallMethod(ospath.py(), "dirname", "O", obj);
}

struct confval : public boost::static_visitor<PyObject*>
{
    PyObject* operator()(double v) const
    {
        return PyFloat_FromDouble(v);
    }

    PyObject* operator()(const std::string& v) const
    {
        return PyString_FromString(v.c_str());
    }

    PyObject* operator()(const std::vector<double>& v) const
    {
        npy_intp dims[]  = {(npy_intp)v.size()};
        PyRef<> obj(PyArray_SimpleNew(1, dims, NPY_DOUBLE));
        std::copy(v.begin(), v.end(), (double*)PyArray_DATA(obj.as<PyArrayObject>()));
        return obj.release();
    }

    PyObject* operator()(const Config::vector_t& v) const
    {
        PyRef<> L(PyList_New(v.size()));

        for(size_t i=0, N=v.size(); i<N; i++)
        {
            PyList_SetItem(L.py(), i, conf2dict(&v[i]));
        }
        return L.release();
    }
};

} // namespace

PyObject* conf2dict(const Config *conf)
{
    PyRef<> ret(PyDict_New());

    for(Config::const_iterator it=conf->begin(), end=conf->end();
        it!=end; ++it)
    {
        if(PyDict_SetItemString(ret.py(), it->first.c_str(),
                                boost::apply_visitor(confval(), it->second)
                                ))
            throw std::runtime_error("Failed to insert into dictionary from conf2dict");
    }

    return ret.release();
}

Config* dict2conf(PyObject *dict)
{
    if(!PyDict_Check(dict))
        throw std::invalid_argument("Not a dict");
    std::auto_ptr<Config> conf(new Config);
    Dict2Config(*conf, dict);
    return conf.release();
}

PyObject* PyGLPSPrint(PyObject *, PyObject *args)
{
    try {
        PyObject *dict;
        if(!PyArg_ParseTuple(args, "O!", &PyDict_Type, &dict))
            return NULL;

        Config conf;
        Dict2Config(conf, dict);
        std::ostringstream strm;
        GLPSPrint(strm, conf);
        return PyString_FromString(strm.str().c_str());
    }CATCH()
}

#ifndef PY_SSIZE_T_CLEAN
#error the following assumes ssize_t is used
#endif

Config* PyGLPSParse2Config(PyObject *, PyObject *args, PyObject *kws)
{
    PyObject *conf = NULL, *extra_defs = Py_None;
    const char *path = NULL;
    const char *pnames[] = {"config", "path", "extra", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, kws, "O|zO", (char**)pnames, &conf, &path, &extra_defs))
        return NULL;

    GLPSParser parser;

    if(extra_defs==Py_None) {
        // no-op
    } else if(PyDict_Check(extra_defs)) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;

        while(PyDict_Next(extra_defs, &pos, &key, &value)) {
            PyRef<> keyx(key, borrow());
            PyCString keystr(keyx);

            Config::value_t curval;

            if(PyNumber_Check(value)) {
                PyRef<> pyf(PyNumber_Float(value));
                curval = PyFloat_AsDouble(pyf.py());

            } else if(PyString_Check(value)) {
                PyRef<> valuex(value, borrow());
                PyCString valstr(valuex);

                curval = valstr.c_str();

            } else {
                PyErr_SetString(PyExc_ValueError, "extra {} can contain only numbers or strings");
                return NULL;
            }

            parser.setVar(keystr.c_str(), curval);
        }
    } else {
        PyErr_SetString(PyExc_ValueError, "'extra' must be a dict");
        return NULL;
    }

    PyGetBuf buf;
    std::auto_ptr<Config> C;

    if(PyDict_Check(conf)) {
        C.reset(dict2conf(conf));

    } else if(PyObject_HasAttrString(conf, "read")) { // file-like
        PyCString pyname;

        if(!path && PyObject_HasAttrString(conf, "name")) {
            path = pyname.c_str(pydirname(PyObject_GetAttrString(conf, "name")));
        }

        PyRef<> pybytes(PyObject_CallMethod(conf, "read", ""));
        if(!buf.get(pybytes.py())) {
            PyErr_SetString(PyExc_TypeError, "read() must return a buffer");
            return NULL;
        }
        C.reset(parser.parse_byte((const char*)buf.data(), buf.size(), path));

    } else if(buf.get(conf)) {
        C.reset(parser.parse_byte((const char*)buf.data(), buf.size(), path));

#if PY_MAJOR_VERSION >= 3
    } else if(PyUnicode_Check(conf)) { // py3 str (aka unicode) doesn't implement buffer iface
        PyCString buf;
        const char *cbuf = buf.c_str(conf);

        C.reset(parser.parse_byte(cbuf, strlen(cbuf), path));
#endif

    } else {
        throw std::invalid_argument("'config' must be dict or byte buffer");
    }

    return C.release();
}

PyObject* PyGLPSParse(PyObject *unused, PyObject *args, PyObject *kws)
{
    try{
        return conf2dict(PyGLPSParse2Config(unused, args, kws));
    }CATCH()
}
