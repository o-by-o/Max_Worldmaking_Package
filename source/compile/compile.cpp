#include "al_max.h"

static t_class * max_class = 0;



class Compile {
public:
	
	typedef int (*testfun_t)(int);
	
	t_object ob; // max objExamplet, must be first!
	
	void * outlet_result;
	
	t_string * code_string;
	testfun_t testfun;
	t_object * clang;
	
	Compile() {
		outlet_result = outlet_new(&ob, 0);
		
		code_string = string_new("#include <stddef.h> \n object_post(0, __VA_ARGS__) \n extern \"C\" int test(int i) { return -i; }");


	}
	
	~Compile() {
		
		if (code_string) object_free(code_string);
		if (clang) object_release(clang);

	}
	
	void test(t_atom_long i) {
		
		clang = (t_object *)object_new(CLASS_NOBOX, gensym("clang"), gensym("ctest"));


		if (clang != NULL) {
			// set C or C++
			object_attr_setlong(clang, gensym("cpp"), 1);

			// add include paths:
			//		object_method(clang, gensym("include"), gensym("path/to/include"));
			//		object_method(clang, gensym("system_include"), gensym("path/to/include")); // for <includes>
			

			// define macros:
#ifdef WIN_VERSION
			object_method(clang, gensym("define"), gensym("WIN_VERSION"));
#endif
#ifdef MAC_VERSION
			object_method(clang, gensym("define"), gensym("MAC_VERSION"));
#endif

			// compile options:
			object_attr_setlong(clang, gensym("vectorize"), 1);
			object_attr_setlong(clang, gensym("fastmath"), 1);

			// push specific symbols:
			object_method(clang, gensym("addsymbol"), gensym("object_post"), &object_post);
			
			t_atom rv, av;
			atom_setobj(&av, code_string);
			object_method_typed(clang, gensym("compile"), 1, &av, &rv);
			int err = atom_getlong(&rv);

			// or, read bitcode:
			// object_method_sym(clang, gensym("readbitcode"), gensym("path to bitcode"));
			
			if (err == 0) {
				
				object_method(clang, gensym("optimize"), gensym("O3"));
				
				// at the point jit is called, this clang object becomes opaque
				// but, must keep it around for as long as any code generated is potentially in use
				// only when all functions become unreachable is it safe to object_release(clang)
				object_method(clang, gensym("jit"));

				// posts to Max console a list of the functions in the module
				//object_method(clang, gensym("listfunctions"));

				// post IR header
				//object_method(clang, gensym("dump"));
				
				// write bitcode:
				//object_method(clang, gensym("writebitcode"), gensym("path to bitcode"));

				// Get a function pointer:
				t_atom fun_atom;
				object_method_sym(clang, gensym("getfunction"), gensym("test"), &fun_atom);
				if (fun_atom.a_w.w_obj) {
					testfun = (testfun_t)atom_getobj(&fun_atom);
				
					int result = testfun((int)i);
					outlet_int(outlet_result, result);
				} else {
					object_error(&ob, "couldn't get function");
				}

				// there is also a "getglobal" that works in the same way as getfunction

				//t_atom ret_atom;
				//object_method_sym(clang, gensym("getdatalayout"), gensym("test"), &ret_atom);
				//post("data layout %s", atom_getsym(&ret_atom)->s_name);
				//object_method_sym(clang, gensym("gettargettriple"), gensym("test"), &ret_atom);
				//post("target triple %s", atom_getsym(&ret_atom)->s_name);
				//object_method_sym(clang, gensym("getmoduleid"), gensym("test"), &ret_atom);
				//post("module id %s", atom_getsym(&ret_atom)->s_name);
				
			} else {
				object_release(clang);
				clang = NULL;
			}
		}
	}
};

void compile_test(Compile *x, t_atom_long i) {
	x->test(i);
}

void * compile_new(t_symbol *s, long argc, t_atom *argv) {
	Compile *x = NULL;
	if ((x = (Compile *)object_alloc(max_class))) {
		
		x = new (x) Compile();
		
		// apply attrs:
		attr_args_process(x, (short)argc, argv);
		
		// invoke any initialization after the attrs are set from here:
		
	}
	return (x);
}

void compile_free(Compile *x) {
	x->~Compile();
}

void compile_assist(Compile *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { // inlet
		sprintf(s, "I am inlet %ld", a);
	}
	else {	// outlet
		sprintf(s, "I am outlet %ld", a);
	}
}

void ext_main(void *r)
{
	t_class *c;
	
	common_symbols_init();
	
	c = class_new("compile", (method)compile_new, (method)compile_free, (long)sizeof(Compile),
				  0L /* leave NULL!! */, A_GIMME, 0);
	
	/* you CAN'T call this from the patcher */
	class_addmethod(c, (method)compile_assist, "assist", A_CANT, 0);
	class_addmethod(c, (method)compile_test, "test", A_LONG, 0);
	
	class_register(CLASS_BOX, c);
	max_class = c;
}
