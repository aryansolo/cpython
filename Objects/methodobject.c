
/* Method object implementation */

#include "Python.h"
#include "internal/mem.h"
#include "internal/pystate.h"
#include "structmember.h"

/* Free list for method objects to safe malloc/free overhead
 * The m_self element is used to chain the objects.
 */
static PyCFunctionObject *free_list = NULL;
static int numfree = 0;
#ifndef PyCFunction_MAXFREELIST
#define PyCFunction_MAXFREELIST 256
#endif

/* undefine macro trampoline to PyCFunction_NewEx */
#undef PyCFunction_New

PyAPI_FUNC(PyObject *)
PyCFunction_New(PyMethodDef *ml, PyObject *self)
{
    return PyCFunction_NewEx(ml, self, NULL);
}


int
_PyCCallDef_FromMethodDef(PyCCallDef *cc, PyMethodDef *ml, PyObject *parent)
{
    assert(cc != NULL);
    assert(ml != NULL);

    PyObject *name = PyUnicode_FromString(ml->ml_name);
    if (name == NULL) {
        return -1;
    }

    /* Compute flags */
    uint32_t flags = CCALL_PROFILE | CCALL_SLICE_SELF;
    switch (ml->ml_flags & METH_SIGNATURE) {
    case METH_VARARGS:
        flags |= CCALL_VARARGS;
        break;
    case METH_FASTCALL:
        flags |= CCALL_FASTCALL;
        break;
    case METH_NOARGS:
        flags |= CCALL_NULLARG;
        break;
    case METH_O:
        flags |= CCALL_O;
        break;
    case METH_VARARGS | METH_KEYWORDS:
        flags |= CCALL_VARARGS | CCALL_KEYWORDS;
        break;
    case METH_FASTCALL | METH_KEYWORDS:
        flags |= CCALL_FASTCALL | CCALL_KEYWORDS;
        break;
    default:
        PyErr_SetString(PyExc_SystemError,
                        "bad call signature flags in PyMethodDef");
        return -1;
    }

    /* Add a very special one-time flag for the print builtin */
    static int enable_print_check = 1;
    if (enable_print_check && strcmp(ml->ml_name, "print") == 0) {
        flags |= _CCALL_BUILTIN_PRINT;
        enable_print_check = 0;
    }

    if (parent != NULL) {
        Py_INCREF(parent);
        if (PyType_Check(parent)) {
            flags |= CCALL_OBJCLASS;
        }
    }

    cc->cc_flags = flags;
    cc->cc_func = (PyCFunc)ml->ml_meth;
    cc->cc_name = name;
    cc->cc_parent = parent;

    return 0;
}


static PyCFunctionObject *
_PyCFunction_NewEmpty(PyTypeObject *cls, PyObject *self, PyObject *module)
{
    PyCFunctionObject *op;
    op = free_list;
    if (op != NULL) {
        free_list = (PyCFunctionObject *)(op->m_self);
        (void)PyObject_INIT(op, cls);
        numfree--;
    }
    else {
        op = PyObject_GC_New(PyCFunctionObject, cls);
        if (op == NULL)
            return NULL;
    }
    Py_XINCREF(self);
    op->m_self = self;
    Py_XINCREF(module);
    op->m_module = module;
    op->m_weakreflist = NULL;
    _PyObject_GC_TRACK(op);
    return op;
}


PyObject *
PyCFunction_NewEx(PyMethodDef *ml, PyObject *self, PyObject *module)
{
    PyObject *parent = self;
    if (self == NULL || (ml->ml_flags & METH_STATIC)) {
        /* Don't set self to NULL: this ensures that the C call protocol
           is backwards compatible with the old behavior */
        self = Py_None;
    }
    return PyCFunction_ClsNew(&PyCFunction_Type, ml, self, module, parent);
}


PyObject *
PyCFunction_ClsNew(PyTypeObject *cls, PyMethodDef *ml, PyObject *self, PyObject *module, PyObject *parent)
{
    PyCFunctionObject *op = _PyCFunction_NewEmpty(cls, self, module);
    if (op == NULL) {
        return NULL;
    }
    op->m_ccall = &op->_ccalldef;

    if (_PyCCallDef_FromMethodDef(&op->_ccalldef, ml, parent) < 0) {
        Py_DECREF(op);
        return NULL;
    }
    op->m_doc = ml->ml_doc;
    return (PyObject *)op;
}


PyObject *
_PyCFunction_NewBoundMethod(PyCFunctionObject *func, PyObject *self)
{
    PyCFunctionObject *op = _PyCFunction_NewEmpty(&PyCFunction_Type, self, NULL);
    if (op == NULL) {
        return NULL;
    }
    PyCCallDef *cc = func->m_ccall;
    Py_INCREF(cc->cc_name);
    Py_XINCREF(cc->cc_parent);
    op->m_ccall = cc;
    op->m_doc = func->m_doc;
    return (PyObject *)op;
}


PyObject *
PyDescr_NewMethod(PyTypeObject *type, PyMethodDef *ml)
{
    return PyCFunction_ClsNew(&PyMethodDescr_Type, ml, NULL, NULL, (PyObject*)type);
}


PyObject *
PyCFunction_GetSelf(PyObject *op)
{
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyCFunction_GET_SELF(op);
}


PyCFunction PyCFunction_GetFunction(PyObject *op)
{
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyCFunction_GET_FUNCTION(op);
}


/* Methods (the standard built-in methods, that is) */

static void
meth_dealloc(PyCFunctionObject *m)
{
    _PyObject_GC_UNTRACK(m);
    if (m->m_weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject*) m);
    }
    Py_DECREF(m->m_ccall->cc_name);
    Py_XDECREF(m->m_ccall->cc_parent);
    Py_XDECREF(m->m_self);
    Py_XDECREF(m->m_module);
    if (numfree < PyCFunction_MAXFREELIST) {
        m->m_self = (PyObject *)free_list;
        free_list = m;
        numfree++;
    }
    else {
        PyObject_GC_Del(m);
    }
}

static PyObject *
meth_reduce(PyCFunctionObject *m, PyObject *Py_UNUSED(ignored))
{

    PyObject *name = m->m_ccall->cc_name;
    PyObject *parent = m->m_self;

    if (parent == NULL || parent == Py_None || PyModule_Check(parent)) {
        parent = m->m_ccall->cc_parent;
        if (parent == NULL || PyModule_Check(parent)) {
            Py_INCREF(name);
            return name;
        }
    }

    _Py_IDENTIFIER(getattr);
    PyObject *builtins = PyEval_GetBuiltins();
    PyObject *getattr = _PyDict_GetItemId(builtins, &PyId_getattr);
    return Py_BuildValue("O(OO)", getattr, parent, name);
}


static PyMethodDef meth_methods[] = {
    {"__reduce__", (PyCFunction)meth_reduce, METH_NOARGS, NULL},
    {NULL, NULL}
};

static PyObject *
meth_get__text_signature__(PyCFunctionObject *m, void *closure)
{
    const char *name = PyUnicode_AsUTF8(m->m_ccall->cc_name);
    if (name == NULL) {
        return NULL;
    }
    return _PyType_GetTextSignatureFromInternalDoc(name, m->m_doc);
}

static PyObject *
meth_get__doc__(PyCFunctionObject *m, void *closure)
{
    const char *name = PyUnicode_AsUTF8(m->m_ccall->cc_name);
    if (name == NULL) {
        return NULL;
    }
    return _PyType_GetDocFromInternalDoc(name, m->m_doc);
}

static int
meth_traverse(PyCFunctionObject *m, visitproc visit, void *arg)
{
    Py_VISIT(m->m_self);
    Py_VISIT(m->m_module);
    Py_VISIT(m->m_ccall->cc_parent);
    return 0;
}

static PyGetSetDef meth_getsets [] = {
    {"__doc__",  (getter)meth_get__doc__,  NULL, NULL},
    {"__text_signature__", (getter)meth_get__text_signature__, NULL, NULL},
    {"__name__", PyCCall_GenericGetName, NULL, NULL},
    {"__qualname__", PyCCall_GenericGetQualname, NULL, NULL},
    {"__self__", PyCCall_GenericGetSelf, NULL, NULL},
    {0}
};

static PyGetSetDef md_getsets [] = {
    {"__doc__",  (getter)meth_get__doc__,  NULL, NULL},
    {"__text_signature__", (getter)meth_get__text_signature__, NULL, NULL},
    {"__name__", PyCCall_GenericGetName, NULL, NULL},
    {"__qualname__", PyCCall_GenericGetQualname, NULL, NULL},
    {"__objclass__", PyCCall_GenericGetParent, NULL, NULL},
    {0}
};

#define OFF(x) offsetof(PyCFunctionObject, x)

static PyMemberDef meth_members[] = {
    {"__module__",    T_OBJECT,     OFF(m_module), PY_WRITE_RESTRICTED},
    {NULL}
};


static PyObject *
meth_repr(PyCFunctionObject *m)
{
    PyObject *name = m->m_ccall->cc_name;
    if (m->m_self == NULL || m->m_self == Py_None || PyModule_Check(m->m_self))
    {
        return PyUnicode_FromFormat("<built-in function %U>", name);
    }
    return PyUnicode_FromFormat("<built-in method %U of %s object at %p>",
                                name,
                                m->m_self->ob_type->tp_name,
                                m->m_self);
}


static PyObject *
md_repr(PyCFunctionObject *m)
{
    PyCCallDef *cc = m->m_ccall;
    return PyUnicode_FromFormat("<method '%U' of '%s' objects>",
        cc->cc_name, ((PyTypeObject*)(cc->cc_parent))->tp_name);
}


static PyObject *
meth_richcompare(PyObject *self, PyObject *other, int op)
{
    PyCFunctionObject *a, *b;
    PyObject *res;

    if ((op != Py_EQ && op != Py_NE) ||
        Py_TYPE(self) != Py_TYPE(other))
    {
        Py_RETURN_NOTIMPLEMENTED;
    }
    a = (PyCFunctionObject *)self;
    b = (PyCFunctionObject *)other;

    int r;
    if (a->m_ccall->cc_func != b->m_ccall->cc_func) {
        r = (op == Py_NE);
    }
    else {
        r = (a->m_self == b->m_self) == (op == Py_EQ);
    }
    res = r ? Py_True : Py_False;
    Py_INCREF(res);
    return res;
}

static Py_hash_t
meth_hash(PyCFunctionObject *m)
{
    Py_uhash_t mult = _PyHASH_MULTIPLIER, err = -1;
    Py_uhash_t h = 0, t;

    if (m->m_self != NULL) {
        h = _Py_HashPointer(m->m_self);
        if (h == err) {
            return -1;
        }
    }

    t = _Py_HashPointer(Py_TYPE(m));
    if (t == err) {
        return -1;
    }
    h = (h * mult) + t;

    t = _Py_HashPointer(m->m_ccall->cc_func);
    if (t == err) {
        return -1;
    }
    h = (h * mult) + t;

    return (h == err) ? err - 1 : h;
}


static PyObject *
meth_get(PyCFunctionObject *m, PyObject *obj, PyObject *type)
{
    PyObject *res;

    /* Already bound or binding to the class => no-op */
    if (m->m_self != NULL || obj == NULL) {
        res = (PyObject*)m;
        Py_INCREF(res);
        return res;
    }

    PyCCallDef *cc = m->m_ccall;
    if (cc->cc_flags & CCALL_OBJCLASS) {
        /* Check __objclass__ */
        PyTypeObject *cls = (PyTypeObject*)cc->cc_parent;
        if (!PyObject_TypeCheck(obj, cls)) {
            PyErr_Format(PyExc_TypeError,
                         "descriptor '%U' for '%s' objects "
                         "doesn't apply to '%s' object",
                         cc->cc_name,
                         cls->tp_name,
                         Py_TYPE(obj)->tp_name);
            return NULL;
        }
    }

    return _PyCFunction_NewBoundMethod(m, obj);
}


PyTypeObject PyCFunction_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "builtin_function_or_method",
    sizeof(PyCFunctionObject),
    0,
    (destructor)meth_dealloc,                   /* tp_dealloc */
    offsetof(PyCFunctionObject, m_ccall),       /* tp_ccalloffset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)meth_repr,                        /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    (hashfunc)meth_hash,                        /* tp_hash */
    PyCCall_Call,                               /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_HAVE_CCALL,                  /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)meth_traverse,                /* tp_traverse */
    0,                                          /* tp_clear */
    meth_richcompare,                           /* tp_richcompare */
    offsetof(PyCFunctionObject, m_weakreflist), /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    meth_methods,                               /* tp_methods */
    meth_members,                               /* tp_members */
    meth_getsets,                               /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    (descrgetfunc)meth_get,                     /* tp_descr_get */
    0,                                          /* tp_descr_set */
};


PyTypeObject PyMethodDescr_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "method_descriptor",
    sizeof(PyCFunctionObject),
    0,
    (destructor)meth_dealloc,                   /* tp_dealloc */
    offsetof(PyCFunctionObject, m_ccall),       /* tp_ccalloffset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)md_repr,                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    (hashfunc)meth_hash,                        /* tp_hash */
    PyCCall_Call,                               /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_HAVE_CCALL,                  /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)meth_traverse,                /* tp_traverse */
    0,                                          /* tp_clear */
    meth_richcompare,                           /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    meth_methods,                               /* tp_methods */
    0,                                          /* tp_members */
    md_getsets,                                 /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    (descrgetfunc)meth_get,                     /* tp_descr_get */
    0,                                          /* tp_descr_set */
};



/* Clear out the free list */

int
PyCFunction_ClearFreeList(void)
{
    int freelist_size = numfree;

    while (free_list) {
        PyCFunctionObject *v = free_list;
        free_list = (PyCFunctionObject *)(v->m_self);
        PyObject_GC_Del(v);
        numfree--;
    }
    assert(numfree == 0);
    return freelist_size;
}

void
PyCFunction_Fini(void)
{
    (void)PyCFunction_ClearFreeList();
}

/* Print summary info about the state of the optimized allocator */
void
_PyCFunction_DebugMallocStats(FILE *out)
{
    _PyDebugAllocatorStats(out,
                           "free PyCFunctionObject",
                           numfree, sizeof(PyCFunctionObject));
}


/* Functions which became macros but are still needed for the stable ABI (PEP 384) */

#undef PyCFunction_Call
PyAPI_FUNC(PyObject *)
PyCFunction_Call(PyObject *func, PyObject *args, PyObject *kwargs)
{
    return PyCCall_Call(func, args, kwargs);
}
