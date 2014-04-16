// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"
#include "accessors.h"

#include "compiler.h"
#include "contexts.h"
#include "deoptimizer.h"
#include "execution.h"
#include "factory.h"
#include "frames-inl.h"
#include "isolate.h"
#include "list-inl.h"
#include "property-details.h"
#include "api.h"

namespace v8 {
namespace internal {


static Handle<AccessorInfo> MakeAccessor(Isolate* isolate,
                                         Handle<String> name,
                                         AccessorGetterCallback getter,
                                         AccessorSetterCallback setter,
                                         PropertyAttributes attributes) {
  Factory* factory = isolate->factory();
  Handle<ExecutableAccessorInfo> info = factory->NewExecutableAccessorInfo();
  info->set_property_attributes(attributes);
  info->set_all_can_read(true);
  info->set_all_can_write(true);
  info->set_prohibits_overwriting(false);
  info->set_name(*name);
  Handle<Object> get = v8::FromCData(isolate, getter);
  Handle<Object> set = v8::FromCData(isolate, setter);
  info->set_getter(*get);
  info->set_setter(*set);
  return info;
}


template <class C>
static C* FindInstanceOf(Isolate* isolate, Object* obj) {
  for (Object* cur = obj; !cur->IsNull(); cur = cur->GetPrototype(isolate)) {
    if (Is<C>(cur)) return C::cast(cur);
  }
  return NULL;
}


// Entry point that never should be called.
MaybeObject* Accessors::IllegalSetter(Isolate* isolate,
                                      JSObject*,
                                      Object*,
                                      void*) {
  UNREACHABLE();
  return NULL;
}


Object* Accessors::IllegalGetAccessor(Isolate* isolate,
                                      Object* object,
                                      void*) {
  UNREACHABLE();
  return object;
}


MaybeObject* Accessors::ReadOnlySetAccessor(Isolate* isolate,
                                            JSObject*,
                                            Object* value,
                                            void*) {
  // According to ECMA-262, section 8.6.2.2, page 28, setting
  // read-only properties must be silently ignored.
  return value;
}


static V8_INLINE bool CheckForName(Handle<String> name,
                                   Handle<String> property_name,
                                   int offset,
                                   int* object_offset) {
  if (String::Equals(name, property_name)) {
    *object_offset = offset;
    return true;
  }
  return false;
}


// Returns true for properties that are accessors to object fields.
// If true, *object_offset contains offset of object field.
template <class T>
bool Accessors::IsJSObjectFieldAccessor(typename T::TypeHandle type,
                                        Handle<String> name,
                                        int* object_offset) {
  Isolate* isolate = name->GetIsolate();

  if (type->Is(T::String())) {
    return CheckForName(name, isolate->factory()->length_string(),
                        String::kLengthOffset, object_offset);
  }

  if (!type->IsClass()) return false;
  Handle<Map> map = type->AsClass();

  switch (map->instance_type()) {
    case JS_ARRAY_TYPE:
      return
        CheckForName(name, isolate->factory()->length_string(),
                     JSArray::kLengthOffset, object_offset);
    case JS_TYPED_ARRAY_TYPE:
      return
        CheckForName(name, isolate->factory()->length_string(),
                     JSTypedArray::kLengthOffset, object_offset) ||
        CheckForName(name, isolate->factory()->byte_length_string(),
                     JSTypedArray::kByteLengthOffset, object_offset) ||
        CheckForName(name, isolate->factory()->byte_offset_string(),
                     JSTypedArray::kByteOffsetOffset, object_offset);
    case JS_ARRAY_BUFFER_TYPE:
      return
        CheckForName(name, isolate->factory()->byte_length_string(),
                     JSArrayBuffer::kByteLengthOffset, object_offset);
    case JS_DATA_VIEW_TYPE:
      return
        CheckForName(name, isolate->factory()->byte_length_string(),
                     JSDataView::kByteLengthOffset, object_offset) ||
        CheckForName(name, isolate->factory()->byte_offset_string(),
                     JSDataView::kByteOffsetOffset, object_offset);
    default:
      return false;
  }
}


template
bool Accessors::IsJSObjectFieldAccessor<Type>(Type* type,
                                              Handle<String> name,
                                              int* object_offset);


template
bool Accessors::IsJSObjectFieldAccessor<HeapType>(Handle<HeapType> type,
                                                  Handle<String> name,
                                                  int* object_offset);


//
// Accessors::ArrayLength
//


MaybeObject* Accessors::ArrayGetLength(Isolate* isolate,
                                       Object* object,
                                       void*) {
  // Traverse the prototype chain until we reach an array.
  JSArray* holder = FindInstanceOf<JSArray>(isolate, object);
  return holder == NULL ? Smi::FromInt(0) : holder->length();
}


// The helper function will 'flatten' Number objects.
Handle<Object> Accessors::FlattenNumber(Isolate* isolate,
                                        Handle<Object> value) {
  if (value->IsNumber() || !value->IsJSValue()) return value;
  Handle<JSValue> wrapper = Handle<JSValue>::cast(value);
  ASSERT(wrapper->GetIsolate()->context()->native_context()->number_function()->
      has_initial_map());
  if (wrapper->map() ==
      isolate->context()->native_context()->number_function()->initial_map()) {
    return handle(wrapper->value(), isolate);
  }

  return value;
}


MaybeObject* Accessors::ArraySetLength(Isolate* isolate,
                                       JSObject* object_raw,
                                       Object* value_raw,
                                       void*) {
  HandleScope scope(isolate);
  Handle<JSObject> object(object_raw, isolate);
  Handle<Object> value(value_raw, isolate);

  // This means one of the object's prototypes is a JSArray and the
  // object does not have a 'length' property.  Calling SetProperty
  // causes an infinite loop.
  if (!object->IsJSArray()) {
    Handle<Object> result;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, result,
        JSObject::SetLocalPropertyIgnoreAttributes(
            object, isolate->factory()->length_string(), value, NONE));
    return *result;
  }

  value = FlattenNumber(isolate, value);

  Handle<JSArray> array_handle = Handle<JSArray>::cast(object);

  Handle<Object> uint32_v;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, uint32_v, Execution::ToUint32(isolate, value));
  Handle<Object> number_v;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, number_v, Execution::ToNumber(isolate, value));

  if (uint32_v->Number() == number_v->Number()) {
    Handle<Object> result;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, result,
        JSArray::SetElementsLength(array_handle, uint32_v));
    return *result;
  }
  return isolate->Throw(
      *isolate->factory()->NewRangeError("invalid_array_length",
                                         HandleVector<Object>(NULL, 0)));
}


const AccessorDescriptor Accessors::ArrayLength = {
  ArrayGetLength,
  ArraySetLength,
  0
};


//
// Accessors::StringLength
//

void Accessors::StringLengthGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* value = *Utils::OpenHandle(*info.This());
  Object* result;
  if (value->IsJSValue()) value = JSValue::cast(value)->value();
  if (value->IsString()) {
    result = Smi::FromInt(String::cast(value)->length());
  } else {
    // If object is not a string we return 0 to be compatible with WebKit.
    // Note: Firefox returns the length of ToString(object).
    result = Smi::FromInt(0);
  }
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(result, isolate)));
}


void Accessors::StringLengthSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::StringLengthInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate,
                      isolate->factory()->length_string(),
                      &StringLengthGetter,
                      &StringLengthSetter,
                      attributes);
}


//
// Accessors::ScriptColumnOffset
//


void Accessors::ScriptColumnOffsetGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* res = Script::cast(JSValue::cast(object)->value())->column_offset();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


void Accessors::ScriptColumnOffsetSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptColumnOffsetInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("column_offset")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptColumnOffsetGetter,
                      &ScriptColumnOffsetSetter,
                      attributes);
}


//
// Accessors::ScriptId
//


void Accessors::ScriptIdGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* id = Script::cast(JSValue::cast(object)->value())->id();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(id, isolate)));
}


void Accessors::ScriptIdSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptIdInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("id")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptIdGetter,
                      &ScriptIdSetter,
                      attributes);
}


//
// Accessors::ScriptName
//


void Accessors::ScriptNameGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* source = Script::cast(JSValue::cast(object)->value())->name();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(source, isolate)));
}


void Accessors::ScriptNameSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptNameInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate,
                      isolate->factory()->name_string(),
                      &ScriptNameGetter,
                      &ScriptNameSetter,
                      attributes);
}


//
// Accessors::ScriptSource
//


void Accessors::ScriptSourceGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* source = Script::cast(JSValue::cast(object)->value())->source();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(source, isolate)));
}


void Accessors::ScriptSourceSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptSourceInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  return MakeAccessor(isolate,
                      isolate->factory()->source_string(),
                      &ScriptSourceGetter,
                      &ScriptSourceSetter,
                      attributes);
}


//
// Accessors::ScriptLineOffset
//


void Accessors::ScriptLineOffsetGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* res = Script::cast(JSValue::cast(object)->value())->line_offset();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


void Accessors::ScriptLineOffsetSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptLineOffsetInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("line_offset")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptLineOffsetGetter,
                      &ScriptLineOffsetSetter,
                      attributes);
}


//
// Accessors::ScriptType
//


void Accessors::ScriptTypeGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* res = Script::cast(JSValue::cast(object)->value())->type();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


void Accessors::ScriptTypeSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptTypeInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("type")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptTypeGetter,
                      &ScriptTypeSetter,
                      attributes);
}


//
// Accessors::ScriptCompilationType
//


void Accessors::ScriptCompilationTypeGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* res = Smi::FromInt(
      Script::cast(JSValue::cast(object)->value())->compilation_type());
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


void Accessors::ScriptCompilationTypeSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptCompilationTypeInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("compilation_type")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptCompilationTypeGetter,
                      &ScriptCompilationTypeSetter,
                      attributes);
}


//
// Accessors::ScriptGetLineEnds
//


void Accessors::ScriptLineEndsGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<Object> object = Utils::OpenHandle(*info.This());
  Handle<Script> script(
      Script::cast(Handle<JSValue>::cast(object)->value()), isolate);
  Script::InitLineEnds(script);
  ASSERT(script->line_ends()->IsFixedArray());
  Handle<FixedArray> line_ends(FixedArray::cast(script->line_ends()));
  // We do not want anyone to modify this array from JS.
  ASSERT(*line_ends == isolate->heap()->empty_fixed_array() ||
         line_ends->map() == isolate->heap()->fixed_cow_array_map());
  Handle<JSArray> js_array =
      isolate->factory()->NewJSArrayWithElements(line_ends);
  info.GetReturnValue().Set(Utils::ToLocal(js_array));
}


void Accessors::ScriptLineEndsSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptLineEndsInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("line_ends")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptLineEndsGetter,
                      &ScriptLineEndsSetter,
                      attributes);
}


//
// Accessors::ScriptGetContextData
//


void Accessors::ScriptContextDataGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  DisallowHeapAllocation no_allocation;
  HandleScope scope(isolate);
  Object* object = *Utils::OpenHandle(*info.This());
  Object* res = Script::cast(JSValue::cast(object)->value())->context_data();
  info.GetReturnValue().Set(Utils::ToLocal(Handle<Object>(res, isolate)));
}


void Accessors::ScriptContextDataSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptContextDataInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("context_data")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptContextDataGetter,
                      &ScriptContextDataSetter,
                      attributes);
}


//
// Accessors::ScriptGetEvalFromScript
//


void Accessors::ScriptEvalFromScriptGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<Object> object = Utils::OpenHandle(*info.This());
  Handle<Script> script(
      Script::cast(Handle<JSValue>::cast(object)->value()), isolate);
  Handle<Object> result = isolate->factory()->undefined_value();
  if (!script->eval_from_shared()->IsUndefined()) {
    Handle<SharedFunctionInfo> eval_from_shared(
        SharedFunctionInfo::cast(script->eval_from_shared()));
    if (eval_from_shared->script()->IsScript()) {
      Handle<Script> eval_from_script(Script::cast(eval_from_shared->script()));
      result = Script::GetWrapper(eval_from_script);
    }
  }

  info.GetReturnValue().Set(Utils::ToLocal(result));
}


void Accessors::ScriptEvalFromScriptSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptEvalFromScriptInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("eval_from_script")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptEvalFromScriptGetter,
                      &ScriptEvalFromScriptSetter,
                      attributes);
}


//
// Accessors::ScriptGetEvalFromScriptPosition
//


void Accessors::ScriptEvalFromScriptPositionGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<Object> object = Utils::OpenHandle(*info.This());
  Handle<Script> script(
      Script::cast(Handle<JSValue>::cast(object)->value()), isolate);
  Handle<Object> result = isolate->factory()->undefined_value();
  if (script->compilation_type() == Script::COMPILATION_TYPE_EVAL) {
    Handle<Code> code(SharedFunctionInfo::cast(
        script->eval_from_shared())->code());
    result = Handle<Object>(
        Smi::FromInt(code->SourcePosition(code->instruction_start() +
                     script->eval_from_instructions_offset()->value())),
        isolate);
  }
  info.GetReturnValue().Set(Utils::ToLocal(result));
}


void Accessors::ScriptEvalFromScriptPositionSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptEvalFromScriptPositionInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("eval_from_script_position")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptEvalFromScriptPositionGetter,
                      &ScriptEvalFromScriptPositionSetter,
                      attributes);
}


//
// Accessors::ScriptGetEvalFromFunctionName
//


void Accessors::ScriptEvalFromFunctionNameGetter(
    v8::Local<v8::String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(info.GetIsolate());
  HandleScope scope(isolate);
  Handle<Object> object = Utils::OpenHandle(*info.This());
  Handle<Script> script(
      Script::cast(Handle<JSValue>::cast(object)->value()), isolate);
  Handle<Object> result;
  Handle<SharedFunctionInfo> shared(
      SharedFunctionInfo::cast(script->eval_from_shared()));
  // Find the name of the function calling eval.
  if (!shared->name()->IsUndefined()) {
    result = Handle<Object>(shared->name(), isolate);
  } else {
    result = Handle<Object>(shared->inferred_name(), isolate);
  }
  info.GetReturnValue().Set(Utils::ToLocal(result));
}


void Accessors::ScriptEvalFromFunctionNameSetter(
    v8::Local<v8::String> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  UNREACHABLE();
}


Handle<AccessorInfo> Accessors::ScriptEvalFromFunctionNameInfo(
      Isolate* isolate, PropertyAttributes attributes) {
  Handle<String> name(isolate->factory()->InternalizeOneByteString(
        STATIC_ASCII_VECTOR("eval_from_function_name")));
  return MakeAccessor(isolate,
                      name,
                      &ScriptEvalFromFunctionNameGetter,
                      &ScriptEvalFromFunctionNameSetter,
                      attributes);
}


//
// Accessors::FunctionPrototype
//


Handle<Object> Accessors::FunctionGetPrototype(Handle<JSFunction> function) {
  CALL_HEAP_FUNCTION(function->GetIsolate(),
                     Accessors::FunctionGetPrototype(function->GetIsolate(),
                                                     *function,
                                                     NULL),
                     Object);
}


Handle<Object> Accessors::FunctionSetPrototype(Handle<JSFunction> function,
                                               Handle<Object> prototype) {
  ASSERT(function->should_have_prototype());
  CALL_HEAP_FUNCTION(function->GetIsolate(),
                     Accessors::FunctionSetPrototype(function->GetIsolate(),
                                                     *function,
                                                     *prototype,
                                                     NULL),
                     Object);
}


MaybeObject* Accessors::FunctionGetPrototype(Isolate* isolate,
                                             Object* object,
                                             void*) {
  JSFunction* function_raw = FindInstanceOf<JSFunction>(isolate, object);
  if (function_raw == NULL) return isolate->heap()->undefined_value();
  while (!function_raw->should_have_prototype()) {
    function_raw = FindInstanceOf<JSFunction>(isolate,
                                              function_raw->GetPrototype());
    // There has to be one because we hit the getter.
    ASSERT(function_raw != NULL);
  }

  if (!function_raw->has_prototype()) {
    HandleScope scope(isolate);
    Handle<JSFunction> function(function_raw);
    Handle<Object> proto = isolate->factory()->NewFunctionPrototype(function);
    JSFunction::SetPrototype(function, proto);
    function_raw = *function;
  }
  return function_raw->prototype();
}


MaybeObject* Accessors::FunctionSetPrototype(Isolate* isolate,
                                             JSObject* object_raw,
                                             Object* value_raw,
                                             void*) {
  JSFunction* function_raw = FindInstanceOf<JSFunction>(isolate, object_raw);
  if (function_raw == NULL) return isolate->heap()->undefined_value();

  HandleScope scope(isolate);
  Handle<JSFunction> function(function_raw, isolate);
  Handle<JSObject> object(object_raw, isolate);
  Handle<Object> value(value_raw, isolate);
  if (!function->should_have_prototype()) {
    // Since we hit this accessor, object will have no prototype property.
    Handle<Object> result;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, result,
        JSObject::SetLocalPropertyIgnoreAttributes(
            object, isolate->factory()->prototype_string(), value, NONE));
    return *result;
  }

  Handle<Object> old_value;
  bool is_observed = *function == *object && function->map()->is_observed();
  if (is_observed) {
    if (function->has_prototype())
      old_value = handle(function->prototype(), isolate);
    else
      old_value = isolate->factory()->NewFunctionPrototype(function);
  }

  JSFunction::SetPrototype(function, value);
  ASSERT(function->prototype() == *value);

  if (is_observed && !old_value->SameValue(*value)) {
    JSObject::EnqueueChangeRecord(
        function, "update", isolate->factory()->prototype_string(), old_value);
  }

  return *function;
}


const AccessorDescriptor Accessors::FunctionPrototype = {
  FunctionGetPrototype,
  FunctionSetPrototype,
  0
};


//
// Accessors::FunctionLength
//


MaybeObject* Accessors::FunctionGetLength(Isolate* isolate,
                                          Object* object,
                                          void*) {
  JSFunction* function = FindInstanceOf<JSFunction>(isolate, object);
  if (function == NULL) return Smi::FromInt(0);
  // Check if already compiled.
  if (function->shared()->is_compiled()) {
    return Smi::FromInt(function->shared()->length());
  }
  // If the function isn't compiled yet, the length is not computed correctly
  // yet. Compile it now and return the right length.
  HandleScope scope(isolate);
  Handle<JSFunction> function_handle(function);
  if (Compiler::EnsureCompiled(function_handle, KEEP_EXCEPTION)) {
    return Smi::FromInt(function_handle->shared()->length());
  }
  return Failure::Exception();
}


const AccessorDescriptor Accessors::FunctionLength = {
  FunctionGetLength,
  ReadOnlySetAccessor,
  0
};


//
// Accessors::FunctionName
//


MaybeObject* Accessors::FunctionGetName(Isolate* isolate,
                                        Object* object,
                                        void*) {
  JSFunction* holder = FindInstanceOf<JSFunction>(isolate, object);
  return holder == NULL
      ? isolate->heap()->undefined_value()
      : holder->shared()->name();
}


const AccessorDescriptor Accessors::FunctionName = {
  FunctionGetName,
  ReadOnlySetAccessor,
  0
};


//
// Accessors::FunctionArguments
//


Handle<Object> Accessors::FunctionGetArguments(Handle<JSFunction> function) {
  CALL_HEAP_FUNCTION(function->GetIsolate(),
                     Accessors::FunctionGetArguments(function->GetIsolate(),
                                                     *function,
                                                     NULL),
                     Object);
}


static MaybeObject* ConstructArgumentsObjectForInlinedFunction(
    JavaScriptFrame* frame,
    Handle<JSFunction> inlined_function,
    int inlined_frame_index) {
  Isolate* isolate = inlined_function->GetIsolate();
  Factory* factory = isolate->factory();
  SlotRefValueBuilder slot_refs(
      frame,
      inlined_frame_index,
      inlined_function->shared()->formal_parameter_count());

  int args_count = slot_refs.args_length();
  Handle<JSObject> arguments =
      factory->NewArgumentsObject(inlined_function, args_count);
  Handle<FixedArray> array = factory->NewFixedArray(args_count);
  slot_refs.Prepare(isolate);
  for (int i = 0; i < args_count; ++i) {
    Handle<Object> value = slot_refs.GetNext(isolate, 0);
    array->set(i, *value);
  }
  slot_refs.Finish(isolate);
  arguments->set_elements(*array);

  // Return the freshly allocated arguments object.
  return *arguments;
}


MaybeObject* Accessors::FunctionGetArguments(Isolate* isolate,
                                             Object* object,
                                             void*) {
  HandleScope scope(isolate);
  JSFunction* holder = FindInstanceOf<JSFunction>(isolate, object);
  if (holder == NULL) return isolate->heap()->undefined_value();
  Handle<JSFunction> function(holder, isolate);

  if (function->shared()->native()) return isolate->heap()->null_value();
  // Find the top invocation of the function by traversing frames.
  List<JSFunction*> functions(2);
  for (JavaScriptFrameIterator it(isolate); !it.done(); it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    frame->GetFunctions(&functions);
    for (int i = functions.length() - 1; i >= 0; i--) {
      // Skip all frames that aren't invocations of the given function.
      if (functions[i] != *function) continue;

      if (i > 0) {
        // The function in question was inlined.  Inlined functions have the
        // correct number of arguments and no allocated arguments object, so
        // we can construct a fresh one by interpreting the function's
        // deoptimization input data.
        return ConstructArgumentsObjectForInlinedFunction(frame, function, i);
      }

      if (!frame->is_optimized()) {
        // If there is an arguments variable in the stack, we return that.
        Handle<ScopeInfo> scope_info(function->shared()->scope_info());
        int index = scope_info->StackSlotIndex(
            isolate->heap()->arguments_string());
        if (index >= 0) {
          Handle<Object> arguments(frame->GetExpression(index), isolate);
          if (!arguments->IsArgumentsMarker()) return *arguments;
        }
      }

      // If there is no arguments variable in the stack or we have an
      // optimized frame, we find the frame that holds the actual arguments
      // passed to the function.
      it.AdvanceToArgumentsFrame();
      frame = it.frame();

      // Get the number of arguments and construct an arguments object
      // mirror for the right frame.
      const int length = frame->ComputeParametersCount();
      Handle<JSObject> arguments = isolate->factory()->NewArgumentsObject(
          function, length);
      Handle<FixedArray> array = isolate->factory()->NewFixedArray(length);

      // Copy the parameters to the arguments object.
      ASSERT(array->length() == length);
      for (int i = 0; i < length; i++) array->set(i, frame->GetParameter(i));
      arguments->set_elements(*array);

      // Return the freshly allocated arguments object.
      return *arguments;
    }
    functions.Rewind(0);
  }

  // No frame corresponding to the given function found. Return null.
  return isolate->heap()->null_value();
}


const AccessorDescriptor Accessors::FunctionArguments = {
  FunctionGetArguments,
  ReadOnlySetAccessor,
  0
};


//
// Accessors::FunctionCaller
//


class FrameFunctionIterator {
 public:
  FrameFunctionIterator(Isolate* isolate, const DisallowHeapAllocation& promise)
      : frame_iterator_(isolate),
        functions_(2),
        index_(0) {
    GetFunctions();
  }
  JSFunction* next() {
    if (functions_.length() == 0) return NULL;
    JSFunction* next_function = functions_[index_];
    index_--;
    if (index_ < 0) {
      GetFunctions();
    }
    return next_function;
  }

  // Iterate through functions until the first occurence of 'function'.
  // Returns true if 'function' is found, and false if the iterator ends
  // without finding it.
  bool Find(JSFunction* function) {
    JSFunction* next_function;
    do {
      next_function = next();
      if (next_function == function) return true;
    } while (next_function != NULL);
    return false;
  }

 private:
  void GetFunctions() {
    functions_.Rewind(0);
    if (frame_iterator_.done()) return;
    JavaScriptFrame* frame = frame_iterator_.frame();
    frame->GetFunctions(&functions_);
    ASSERT(functions_.length() > 0);
    frame_iterator_.Advance();
    index_ = functions_.length() - 1;
  }
  JavaScriptFrameIterator frame_iterator_;
  List<JSFunction*> functions_;
  int index_;
};


MaybeObject* Accessors::FunctionGetCaller(Isolate* isolate,
                                          Object* object,
                                          void*) {
  HandleScope scope(isolate);
  DisallowHeapAllocation no_allocation;
  JSFunction* holder = FindInstanceOf<JSFunction>(isolate, object);
  if (holder == NULL) return isolate->heap()->undefined_value();
  if (holder->shared()->native()) return isolate->heap()->null_value();
  Handle<JSFunction> function(holder, isolate);

  FrameFunctionIterator it(isolate, no_allocation);

  // Find the function from the frames.
  if (!it.Find(*function)) {
    // No frame corresponding to the given function found. Return null.
    return isolate->heap()->null_value();
  }

  // Find previously called non-toplevel function.
  JSFunction* caller;
  do {
    caller = it.next();
    if (caller == NULL) return isolate->heap()->null_value();
  } while (caller->shared()->is_toplevel());

  // If caller is a built-in function and caller's caller is also built-in,
  // use that instead.
  JSFunction* potential_caller = caller;
  while (potential_caller != NULL && potential_caller->IsBuiltin()) {
    caller = potential_caller;
    potential_caller = it.next();
  }
  if (!caller->shared()->native() && potential_caller != NULL) {
    caller = potential_caller;
  }
  // If caller is bound, return null. This is compatible with JSC, and
  // allows us to make bound functions use the strict function map
  // and its associated throwing caller and arguments.
  if (caller->shared()->bound()) {
    return isolate->heap()->null_value();
  }
  // Censor if the caller is not a sloppy mode function.
  // Change from ES5, which used to throw, see:
  // https://bugs.ecmascript.org/show_bug.cgi?id=310
  if (caller->shared()->strict_mode() == STRICT) {
    return isolate->heap()->null_value();
  }

  return caller;
}


const AccessorDescriptor Accessors::FunctionCaller = {
  FunctionGetCaller,
  ReadOnlySetAccessor,
  0
};


//
// Accessors::MakeModuleExport
//

static void ModuleGetExport(
    v8::Local<v8::String> property,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  JSModule* instance = JSModule::cast(*v8::Utils::OpenHandle(*info.Holder()));
  Context* context = Context::cast(instance->context());
  ASSERT(context->IsModuleContext());
  int slot = info.Data()->Int32Value();
  Object* value = context->get(slot);
  Isolate* isolate = instance->GetIsolate();
  if (value->IsTheHole()) {
    Handle<String> name = v8::Utils::OpenHandle(*property);
    isolate->ScheduleThrow(
        *isolate->factory()->NewReferenceError("not_defined",
                                               HandleVector(&name, 1)));
    return;
  }
  info.GetReturnValue().Set(v8::Utils::ToLocal(Handle<Object>(value, isolate)));
}


static void ModuleSetExport(
    v8::Local<v8::String> property,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  JSModule* instance = JSModule::cast(*v8::Utils::OpenHandle(*info.Holder()));
  Context* context = Context::cast(instance->context());
  ASSERT(context->IsModuleContext());
  int slot = info.Data()->Int32Value();
  Object* old_value = context->get(slot);
  if (old_value->IsTheHole()) {
    Handle<String> name = v8::Utils::OpenHandle(*property);
    Isolate* isolate = instance->GetIsolate();
    isolate->ScheduleThrow(
        *isolate->factory()->NewReferenceError("not_defined",
                                               HandleVector(&name, 1)));
    return;
  }
  context->set(slot, *v8::Utils::OpenHandle(*value));
}


Handle<AccessorInfo> Accessors::MakeModuleExport(
    Handle<String> name,
    int index,
    PropertyAttributes attributes) {
  Isolate* isolate = name->GetIsolate();
  Factory* factory = isolate->factory();
  Handle<ExecutableAccessorInfo> info = factory->NewExecutableAccessorInfo();
  info->set_property_attributes(attributes);
  info->set_all_can_read(true);
  info->set_all_can_write(true);
  info->set_name(*name);
  info->set_data(Smi::FromInt(index));
  Handle<Object> getter = v8::FromCData(isolate, &ModuleGetExport);
  Handle<Object> setter = v8::FromCData(isolate, &ModuleSetExport);
  info->set_getter(*getter);
  if (!(attributes & ReadOnly)) info->set_setter(*setter);
  return info;
}


} }  // namespace v8::internal
