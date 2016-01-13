#include "eval.h"
#include "env.h"
#include "assertions.h"
#include "reader.h"
#include "gc.h"

#define LOG_STACK 0
#define SHOW_MACRO_EXPANSION 0
#define LOG_FUNC_APPLICATION 0

#define STACK_TRACE_LEN 256
char function_trace[STACK_SIZE][STACK_TRACE_LEN];
int function_trace_pos;

void stack_print() {
  printf("----- STACK -----\n");
  for(int i = 0; i < stack_pos; i++) {
    printf("%d\t%s\n", i, obj_to_string(stack[i])->s);
  }
  printf("-----  END  -----\n\n");
}

void stack_push(Obj *o) {
  if(LOG_STACK) {
    printf("Pushing %s\n", obj_to_string(o)->s);
  }
  if(stack_pos >= STACK_SIZE) {
    printf("Stack overflow.");
    exit(1);
  }
  stack[stack_pos++] = o;
  if(LOG_STACK) {
    stack_print();
  }
}

Obj *stack_pop() {
  if(error) {
    return nil;
  }
  if(stack_pos <= 0) {
    printf("Stack underflow.");
    assert(false);
  }
  if(LOG_STACK) {
    printf("Popping %s\n", obj_to_string(stack[stack_pos - 1])->s);
  }
  Obj *o = stack[--stack_pos];
  if(LOG_STACK) {
    stack_print();
  }
  return o;
}

void function_trace_print() {
  printf("     -----------------\n");
  for(int i = function_trace_pos - 1; i >= 0; i--) {
    printf("%3d  %s\n", i, function_trace[i]);
  }
  printf("     -----------------\n");
}

bool obj_match(Obj *env, Obj *attempt, Obj *value);

bool obj_match_lists(Obj *env, Obj *attempt, Obj *value) {
  //printf("Matching list %s with %s\n", obj_to_string(attempt)->s, obj_to_string(value)->s);
  Obj *p1 = attempt;
  Obj *p2 = value;
  while(p1 && p1->car) {
    if(obj_eq(p1->car, ampersand) && p1->cdr && p1->cdr->car) {
      //printf("Matching & %s against %s\n", obj_to_string(p1->cdr->car)->s, obj_to_string(p2)->s);
      bool matched_rest = obj_match(env, p1->cdr->car, p2);
      return matched_rest;
    }
    else if(!p2 || !p2->car) {
      return false;
    }
    bool result = obj_match(env, p1->car, p2->car);
    if(!result) {
      return false;
    }
    p1 = p1->cdr;
    p2 = p2->cdr;
  }
  if(p2 && p2->car) {
    return false;
  }
  else {
    return true;
  }
}

bool obj_match(Obj *env, Obj *attempt, Obj *value) {

  if(attempt->tag == 'C' && obj_eq(attempt->car, lisp_quote) && attempt->cdr && attempt->cdr->car) {
    // Dubious HACK to enable matching on quoted things...
    // Don't want to extend environment in this case!
    Obj *quoted_attempt = attempt->cdr->car;
    return obj_eq(quoted_attempt, value);
  }
  else if(attempt->tag == 'Y') {
    //printf("Binding %s to value %s in match.\n", obj_to_string(attempt)->s, obj_to_string(value)->s);
    env_extend(env, attempt, value);
    return true;
  }
  else if(attempt->tag == 'C' && value->tag == 'C') {
    return obj_match_lists(env, attempt, value);
  }
  else if(obj_eq(attempt, value)) {
    return true;
  }
  else {
    /* printf("attempt %s (%c) is NOT equal to value %s (%c)\n", */
    /* 	   obj_to_string(attempt)->s, */
    /* 	   attempt->tag, */
    /* 	   obj_to_string(value)->s, */
    /* 	   value->tag); */
    return false;
  }
}

void match(Obj *env, Obj *value, Obj *attempts) {
  Obj *p = attempts;
  while(p && p->car) {
    //printf("\nWill match %s with value %s\n", obj_to_string(p->car)->s, obj_to_string(value)->s);
    Obj *new_env = obj_new_environment(env);
    bool result = obj_match(new_env, p->car, value);

    if(result) {
      //printf("Match found, evaling %s in env\n", obj_to_string(p->cdr->car)->s); //, obj_to_string(new_env)->s);
      eval_internal(new_env, p->cdr->car); // eval the following form using the new environment
      return;
    }
    
    if(!p->cdr) {
      set_error("Uneven nr of forms in match.", attempts);
    }
      
    p = p->cdr->cdr;
  }

  set_error("Failed to find a suitable match for: ", value);
}

void eval_text(Obj *env, char *text, bool print) {
  Obj *forms = read_string(env, text);
  Obj *form = forms;
  while(form && form->car) {
    Obj *result = eval(env, form->car);
    if(error) {
      printf("\e[31mERROR: %s\e[0m\n", obj_to_string_not_prn(error)->s);
      function_trace_print();
      error = NULL;
      gc(env, NULL);
      return;
    }
    if(print) {
      if(result) {
	obj_print(result);
      }
      else {
	printf("Result was NULL when evaling %s\n", obj_to_string(form->car)->s);
      }
      printf("\n");
    }
    form = form->cdr;
    gc(env, forms);
  }
}

void apply(Obj *function, Obj **args, int arg_count) {
  if(function->tag == 'L') {
    Obj *calling_env = obj_new_environment(function->env);
    //printf("Calling function that has parameters: %s\n", obj_to_string(function->params)->s);
    env_extend_with_args(calling_env, function, arg_count, args);
    //printf("Lambda env: %s\n", obj_to_string(calling_env)->s);
    eval_internal(calling_env, function->body);
  }
  else if(function->tag == 'P') {
    Obj *result = function->primop(args, arg_count);
    stack_push(result);
  }
  else if(function->tag == 'F') {
    assert(function);

    if(!function->funptr) {
      error = obj_new_string("Can't call foregin function, it's funptr is NULL. May be a stub function with just a signature?");
      return;
    }
    
    assert(function->cif);
    assert(function->arg_types);
    assert(function->return_type);
     
    void *values[arg_count];

    Obj *p = function->arg_types;
    for(int i = 0; i < arg_count; i++) {
      if(p && p->cdr) {
	assert(p->car);
	if(obj_eq(p->car, type_int)) {
	  assert_or_set_error(args[i]->tag == 'I', "Invalid type of arg: ", args[i]);
	  values[i] = &args[i]->i;
	}
	else if(obj_eq(p->car, type_float)) {
	  assert_or_set_error(args[i]->tag == 'V', "Invalid type of arg: ", args[i]);
	  values[i] = &args[i]->f32;
	}
	else if(obj_eq(p->car, type_string)) {
	  assert_or_set_error(args[i]->tag == 'S', "Invalid type of arg: ", args[i]);
	  values[i] = &args[i]->s;
	}
	else if(p->car->tag == 'C' && obj_eq(p->car->car, obj_new_keyword("ptr"))) { // TODO: replace with a shared keyword to avoid allocs
	  assert_or_set_error(args[i]->tag == 'Q', "Invalid type of arg: ", args[i]);
	  values[i] = &args[i]->void_ptr;
	}
	else {
	  set_error("Can't call foreign function with argument of type ", p->car);
	}
	p = p->cdr;
      }
      else {
	set_error("Too many arguments to ", function);
      }	
    }

    if(p && p->car) {
      set_error("Too few arguments to ", function);
    }

    Obj *obj_result = NULL;
    
    if(obj_eq(function->return_type, type_string)) {
      //printf("Returning string.\n");
      char *c = NULL;
      ffi_call(function->cif, function->funptr, &c, values);

      if(c == NULL) {
	//printf("c is null");
	obj_result = obj_new_string("");
      }
      else {      
	obj_result = obj_new_string(c);
      }
    }
    else if(obj_eq(function->return_type, type_int)) { 
      //printf("Returning int.\n");
      int result;
      ffi_call(function->cif, function->funptr, &result, values);
      obj_result = obj_new_int(result);
    }
    else if(obj_eq(function->return_type, type_bool)) { 
      //printf("Returning bool.\n");
      int result;
      ffi_call(function->cif, function->funptr, &result, values);
      obj_result = result ? lisp_true : lisp_false;
    }
    else if(obj_eq(function->return_type, type_float)) { 
      //printf("Returning float.\n");
      float result;
      ffi_call(function->cif, function->funptr, &result, values);
      obj_result = obj_new_float(result);
    }
    else if(obj_eq(function->return_type, type_void)) { 
      //printf("Returning void.\n");
      int result;
      ffi_call(function->cif, function->funptr, &result, values);
      obj_result = nil;
    }
    else if(function->return_type->tag == 'C' && obj_eq(function->return_type->car, type_ptr)) {
      void *result;
      ffi_call(function->cif, function->funptr, &result, values);
      //printf("Creating new void* with value: %p\n", result);
      obj_result = obj_new_ptr(result);
    }
    else {
      set_error("Returning what? ", function->return_type);
    }

    assert(obj_result);
    stack_push(obj_result);
  }
  else if(function->tag == 'K') {
    if(arg_count != 1) {
      error = obj_new_string("Args to keyword lookup must be a single arg.");
    }
    else if(args[0]->tag != 'E') {
      error = obj_new_string("Arg 0 to keyword lookup must be a dictionary: ");
      obj_string_mut_append(error, obj_to_string(args[0])->s);
    }
    else {
      Obj *value = env_lookup(args[0], function);
      if(value) {
	stack_push(value);
      } else {
	error = obj_new_string("Failed to lookup keyword '");
	obj_string_mut_append(error, obj_to_string(function)->s);
	obj_string_mut_append(error, "'");
      }
    }
  }
  else {
    set_error("Can't call non-function: ", function);
  }
}

#define HEAD_EQ(str) (o->car->tag == 'Y' && strcmp(o->car->s, (str)) == 0)

void eval_list(Obj *env, Obj *o) {
  assert(o);
  //printf("Evaling list %s\n", obj_to_string(o)->s);
  if(!o->car) {
    stack_push(o); // nil, empty list
  }
  else if(HEAD_EQ("do")) {
    Obj *p = o->cdr;
    while(p && p->car) {
      eval_internal(env, p->car);
      if(error) { return; }
      p = p->cdr;
      if(p && p->car) {
	stack_pop(); // remove result from form that is not last
      }
    }
  }
  else if(HEAD_EQ("let")) {
    Obj *let_env = obj_new_environment(env);
    Obj *p = o->cdr->car;
    assert_or_set_error(o->cdr->car, "No bindings in 'let' form.", o);
    while(p && p->car) {
      if(!p->cdr) {
	set_error("Uneven nr of forms in let: ", o);
      }
      assert_or_set_error(p->car->tag == 'Y', "Must bind to symbol in let form: ", p->car);
      eval_internal(let_env, p->cdr->car);
      if(error) { return; }
      env_extend(let_env, p->car, stack_pop());
      p = p->cdr->cdr;
    }
    assert_or_set_error(o->cdr->cdr->car, "No body in 'let' form.", o);
    eval_internal(let_env, o->cdr->cdr->car);
  }
  else if(HEAD_EQ("not")) {
    Obj *p = o->cdr;
    while(p) {
      if(p->car) {
	eval_internal(env, p->car);
	if(error) { return; }
	if(is_true(stack_pop())) {
	  stack_push(lisp_false);
	  return;
	}
      }
      p = p->cdr;
    }
    stack_push(lisp_true);
  }
  else if(HEAD_EQ("quote")) {
    if(o->cdr == nil) {
      stack_push(nil);
    } else {
      stack_push(o->cdr->car);
    }
  }
  else if(HEAD_EQ("while")) {
    eval_internal(env, o->cdr->car);
    if(error) {
      return;
    }
    while(is_true(stack_pop())) {
      eval_internal(env, o->cdr->cdr->car);
      stack_pop();
      eval_internal(env, o->cdr->car);
      if(error) {
	return;
      }
    }
    stack_push(nil);
  }
  else if(HEAD_EQ("if")) {
    eval_internal(env, o->cdr->car);
    if(error) {
      return;
    }
    else if(is_true(stack_pop())) {
      eval_internal(env, o->cdr->cdr->car);
    }
    else {
      eval_internal(env, o->cdr->cdr->cdr->car);
    }
  }
  else if(HEAD_EQ("match")) {
    eval_internal(env, o->cdr->car);
    if(error) { return; }
    Obj *value = stack_pop();
    Obj *p = o->cdr->cdr;   
    match(env, value, p);
  }
  else if(HEAD_EQ("reset!")) {
    assert_or_set_error(o->cdr->car->tag == 'Y', "Must use 'reset!' on a symbol.", o->cdr->car);
    Obj *pair = env_lookup_binding(env, o->cdr->car);
    if(!pair->car || pair->car->tag != 'Y') {
      printf("Can't reset! binding '%s', it's '%s'\n", o->cdr->car->s, obj_to_string(pair)->s);
      stack_push(nil);
      return;
    }
    eval_internal(env, o->cdr->cdr->car);
    if(error) { return; }
    pair->cdr = stack_pop();
    stack_push(pair->cdr);
  }
  else if(HEAD_EQ("fn")) {
    assert_or_set_error(o->cdr, "Lambda form too short (no parameter list or body).", o);
    assert_or_set_error(o->cdr->car, "No parameter list in lambda.", o);
    Obj *params = o->cdr->car;
    assert_or_set_error(o->cdr->cdr, "Lambda form too short (no body).", o);
    assert_or_set_error(o->cdr->cdr->car, "No body in lambda: ", o);
    Obj *body = o->cdr->cdr->car;
    //printf("Creating lambda with env: %s\n", obj_to_string(env)->s);
    Obj *lambda = obj_new_lambda(params, body, env, o);
    stack_push(lambda);
  }
  else if(HEAD_EQ("macro")) {
    assert_or_set_error(o->cdr, "Macro form too short (no parameter list or body): ", o);
    assert_or_set_error(o->cdr->car, "No parameter list in macro: ", o);
    Obj *params = o->cdr->car;
    assert_or_set_error(o->cdr->cdr, "Macro form too short (no body): ", o);
    assert_or_set_error(o->cdr->cdr->car, "No body in macro: ", o);
    Obj *body = o->cdr->cdr->car;
    Obj *macro = obj_new_macro(params, body, env, o);
    stack_push(macro);
  }
  else if(HEAD_EQ("def")) {
    assert_or_set_error(o->cdr, "Too few args to 'def': ", o);
    assert_or_set_error(o->cdr->car, "Can't assign to nil: ", o);
    assert_or_set_error(o->cdr->car->tag == 'Y', "Can't assign to non-symbol: ", o);
    Obj *key = o->cdr->car;
    eval_internal(env, o->cdr->cdr->car); // eval the second arg to 'def', the value to assign
    if(error) { return; } // don't define it if there was an error
    Obj *val = stack_pop();
    global_env_extend(key, val);
    //printf("def %s to %s\n", obj_to_string(key)->s, obj_to_string(val)->s);
    stack_push(val);
  }
  else if(HEAD_EQ("def?")) {
    Obj *key = o->cdr->car;
    if(obj_eq(nil, env_lookup_binding(env, key))) {
      stack_push(lisp_false);
    } else {
      stack_push(lisp_true);
    }
  }
  else {
    // Lambda, primop or macro
    eval_internal(env, o->car);
    if(error) { return; }
    
    Obj *function = stack_pop();
    assert_or_set_error(function, "Can't call NULL.", o);
    
    bool eval_args = function->tag != 'M'; // macros don't eval their args
    Obj *p = o->cdr;
    int count = 0;
    
    while(p && p->car) {
      if(error) {
	return;
      }
      
      if(eval_args) {
	eval_internal(env, p->car);
      }
      else {
	stack_push(p->car); // push non-evaled
      }
      count++;
      p = p->cdr;
    }

    if(error) {
      return;
    }
    
    Obj *args[count];
    for(int i = 0; i < count; i++) {
      Obj *arg = stack_pop();
      args[count - i - 1] = arg;
    }

    if(function->tag == 'M') {
      Obj *calling_env = obj_new_environment(function->env);
      env_extend_with_args(calling_env, function, count, args);
      eval_internal(calling_env, function->body);
      if(error) { return; }
      Obj *expanded = stack_pop();
      if(SHOW_MACRO_EXPANSION) {
	printf("Expanded macro: %s\n", obj_to_string(expanded)->s);
      }
      eval_internal(env, expanded);
    }
    else {
      if(function_trace_pos > STACK_SIZE - 1) {
	printf("Out of function trace stack.\n");
	stack_print();
	function_trace_print();
	exit(1);
      }

      if(LOG_FUNC_APPLICATION) {
	printf("evaluating form %s\n", obj_to_string(o)->s);
      }
      
      snprintf(function_trace[function_trace_pos], STACK_TRACE_LEN, "%s", obj_to_string(o)->s);
      function_trace_pos++;
      
      apply(function, args, count);
      if(!error) {
	function_trace_pos--;
      }
    }
  }
}

void eval_internal(Obj *env, Obj *o) {
  if(error) { return; }

  //printf("%s\n", obj_to_string(o)->s);
  
  if(!o) {
    stack_push(nil);
  }
  else if(o->tag == 'C') {
    eval_list(env, o);
  }
  else if(o->tag == 'E') {
    Obj *new_env = obj_copy(o);
    Obj *p = new_env->bindings;
    while(p && p->car) {
      Obj *pair = p->car;
      eval_internal(env, pair->cdr);
      //printf("Evaling env-binding %s, setting cdr to %s.\n", obj_to_string(pair)->s, obj_to_string(stack[stack_pos - 1])->s);
      pair->cdr = stack_pop();
      p = p->cdr;
    }
    stack_push(new_env);
  }
  else if(o->tag == 'Y') {
    Obj *result = env_lookup(env, o);
    if(!result) {
      char buffer[256];
      snprintf(buffer, 256, "Can't find '%s' in environment.", obj_to_string(o)->s);
      error = obj_new_string(buffer);
      stack_push(nil);
    } else {
      stack_push(result);
    }
  }
  else {
    stack_push(o);
  }
}

Obj *eval(Obj *env, Obj *form) {
  error = NULL;
  stack_pos = 0;
  function_trace_pos = 0;
  eval_internal(env, form);
  Obj *result = stack_pop();
  return result;
}


