/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <string.h>
#include "jvmti.h"

extern "C" {

#define MAX_FRAME_COUNT 20

static jvmtiEnv *jvmti = NULL;
static jthread exp_thread = NULL;
static jrawMonitorID event_mon = NULL;
static int breakpoint_count = 0;
static int frame_pop_count = 0;
static int method_entry_count = 0;
static int method_exit_count = 0;
static int single_step_count = 0;

static void
lock_events() {
  jvmti->RawMonitorEnter(event_mon);
}

static void
unlock_events() {
  jvmti->RawMonitorExit(event_mon);
}

static void
check_jvmti_status(JNIEnv* jni, jvmtiError err, const char* msg) {
  if (err != JVMTI_ERROR_NONE) {
    printf("check_jvmti_status: JVMTI function returned error: %d\n", err);
    jni->FatalError(msg);
  }
}

static char* get_method_class_name(jvmtiEnv *jvmti, JNIEnv* jni, jmethodID method) {
  jvmtiError err;
  jclass klass = NULL;
  char*  cname = NULL;

  err = jvmti->GetMethodDeclaringClass(method, &klass);
  check_jvmti_status(jni, err, "get_method_class_name: error in JVMTI GetMethodDeclaringClass");

  err = jvmti->GetClassSignature(klass, &cname, NULL);
  check_jvmti_status(jni, err, "get_method_class_name: error in JVMTI GetClassSignature");

  cname[strlen(cname) - 1] = '\0'; // get rid of trailing ';'
  return cname + 1;                // get rid of leading 'L'
}

static void
print_method(jvmtiEnv *jvmti, JNIEnv* jni, jmethodID method, jint depth) {
  char*  cname = NULL;
  char*  mname = NULL;
  char*  msign = NULL;
  jvmtiError err;

  cname = get_method_class_name(jvmti, jni, method);

  err = jvmti->GetMethodName(method, &mname, &msign, NULL);
  check_jvmti_status(jni, err, "print_method: error in JVMTI GetMethodName");

  printf("%2d: %s: %s%s\n", depth, cname, mname, msign);
  fflush(0);
}

static void
print_stack_trace(jvmtiEnv *jvmti, JNIEnv* jni) { 
  jvmtiFrameInfo frames[MAX_FRAME_COUNT];
  jint count = 0;
  jvmtiError err;

  err = jvmti->GetStackTrace(NULL, 0, MAX_FRAME_COUNT, frames, &count);
  check_jvmti_status(jni, err, "print_stack_trace: error in JVMTI GetStackTrace");

  printf("JVMTI Stack Trace: frame count: %d\n", count);
  for (int depth = 0; depth < count; depth++) {
    print_method(jvmti, jni, frames[depth].method, depth);
  }
  printf("\n");
}

static void
print_frame_event_info(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jmethodID method,
                       const char* event_name, int event_count) {
  char* cname = NULL;
  char* mname = NULL;
  char* msign = NULL;
  jvmtiThreadInfo thr_info;
  jvmtiError err;

  memset(&thr_info, 0, sizeof(thr_info));
  err = jvmti->GetThreadInfo(thread, &thr_info);
  check_jvmti_status(jni, err, "event handler: error in JVMTI GetThreadInfo call");
  const char* thr_name = (thr_info.name == NULL) ? "<Unnamed thread>" : thr_info.name;

  cname = get_method_class_name(jvmti, jni, method);

  err = jvmti->GetMethodName(method, &mname, &msign, NULL);
  check_jvmti_status(jni, err, "event handler: error in JVMTI GetMethodName call");

  printf("\n%s event #%d: thread: %s, method: %s: %s%s\n",
         event_name, event_count, thr_name, cname, mname, msign);

  if (strcmp(event_name, "SingleStep") != 0) {
    print_stack_trace(jvmti, jni);
  }
  fflush(0);
}

static void
print_cont_event_info(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jint frames_cnt, const char* event_name) {
  jvmtiThreadInfo thr_info;
  jvmtiError err;

  memset(&thr_info, 0, sizeof(thr_info));
  err = jvmti->GetThreadInfo(thread, &thr_info);
  check_jvmti_status(jni, err, "event handler failed during JVMTI GetThreadInfo call");

  const char* thr_name = (thr_info.name == NULL) ? "<Unnamed thread>" : thr_info.name;
  printf("\n%s event: thread: %s, frames: %d\n\n", event_name, thr_name, frames_cnt);

  print_stack_trace(jvmti, jni);
  fflush(0);
}

static void JNICALL
MethodEntry(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jmethodID method) {
  char* mname = NULL;
  jvmtiError err;

  lock_events();

  err = jvmti->GetMethodName(method, &mname, NULL, NULL);
  check_jvmti_status(jni, err, "MethodEntry: error in JVMTI GetMethodName call");

  if (strcmp(mname, "getNextFib") != 0) {
    return; // ignore unrelated events
  }

  printf("\nMethodEntry: Requesting FramePop notifications for top frame\n");

  err = jvmti->NotifyFramePop(thread, 0);
  check_jvmti_status(jni, err, "MethodEntry: error in JVMTI NotifyFramePop");

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_FRAME_POP, thread);
  check_jvmti_status(jni, err, "MethodEntry: error in JVMTI SetEventNotificationMode: enable FRAME_POP");

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_METHOD_EXIT, thread);
  check_jvmti_status(jni, err, "MethodEntry: error in JVMTI SetEventNotificationMode: enable METHOD_EXIT");

  print_frame_event_info(jvmti, jni, thread, method,
                         "MethodEntry", ++method_entry_count);

  unlock_events();
}

static void JNICALL
MethodExit(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jmethodID method,
           jboolean was_popped_by_exception, jvalue return_value) {
  char* mname = NULL;
  jvmtiError err;

  lock_events();

  err = jvmti->GetMethodName(method, &mname, NULL, NULL);
  check_jvmti_status(jni, err, "MethodExit: error in JVMTI GetMethodName call");

  if (strcmp(mname, "getNextFib") != 0) {
    return; // ignore unelated events
  }
  print_frame_event_info(jvmti, jni, thread, method,
                         "MethodExit", ++method_exit_count);

  err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_METHOD_EXIT, thread);
  check_jvmti_status(jni, err, "MethodExit: error in JVMTI SetEventNotificationMode: disable METHOD_EXIT");

  unlock_events();
}

static void JNICALL
Breakpoint(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread,
           jmethodID method, jlocation location) {
  char* mname = NULL;
  jvmtiError err;

  lock_events();

  err = jvmti->GetMethodName(method, &mname, NULL, NULL);
  check_jvmti_status(jni, err, "Breakpoint: error in JVMTI GetMethodName call");

  if (strcmp(mname, "fibTest") != 0) {
    return; // ignore unrelated events
  }
  print_frame_event_info(jvmti, jni, thread, method,
                         "Breakpoint", ++breakpoint_count);

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_SINGLE_STEP, thread);
  check_jvmti_status(jni, err, "Breakpoint: error in JVMTI SetEventNotificationMode: enable SINGLE_STEP");

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_METHOD_ENTRY, thread);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI SetEventNotificationMode: enable METHOD_ENTRY");

  unlock_events();
}

static void JNICALL
SingleStep(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread,
           jmethodID method, jlocation location) {
  char* mname = NULL;
  jvmtiError err;

  lock_events();

  err = jvmti->GetMethodName(method, &mname, NULL, NULL);
  check_jvmti_status(jni, err, "SingleStep: error in JVMTI GetMethodName call");

  if (strcmp(mname, "getNextFib") != 0) {
    return; // ignore unrelated events 
  }
  print_frame_event_info(jvmti, jni, thread, method,
                         "SingleStep", ++single_step_count);

  unlock_events();
}

static void JNICALL
FramePop(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread,
         jmethodID method, jboolean was_popped_by_exception) {
  char* mname = NULL;
  jvmtiError err;

  lock_events();

  err = jvmti->GetMethodName(method, &mname, NULL, NULL);
  check_jvmti_status(jni, err, "FramePop: error in JVMTI GetMethodName call");

  if (strcmp(mname, "getNextFib") != 0) {
    return; // ignore unrelated events
  }

  print_frame_event_info(jvmti, jni, thread, method,
                         "FramePop", ++frame_pop_count);

  err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_SINGLE_STEP, NULL);
  check_jvmti_status(jni, err, "FramePop: error in JVMTI SetEventNotificationMode: disable SINGLE_STEP");

  err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_FRAME_POP, NULL);
  check_jvmti_status(jni, err, "FramePop: error in JVMTI SetEventNotificationMode: disable FRAME_POP");

  unlock_events();
}

JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
  jvmtiEventCallbacks callbacks;
  jvmtiCapabilities caps;
  jvmtiError err;

  printf("Agent_OnLoad started\n");
  if (jvm->GetEnv((void **) (&jvmti), JVMTI_VERSION) != JNI_OK) {
    return JNI_ERR;
  }

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.Breakpoint  = &Breakpoint;
  callbacks.FramePop    = &FramePop;
  callbacks.MethodEntry = &MethodEntry;
  callbacks.MethodExit  = &MethodExit;
  callbacks.SingleStep  = &SingleStep;

  memset(&caps, 0, sizeof(caps));
  caps.can_generate_breakpoint_events = 1;
  caps.can_generate_frame_pop_events = 1;
  caps.can_generate_method_entry_events = 1;
  caps.can_generate_method_exit_events = 1;
  caps.can_generate_single_step_events = 1;

  err = jvmti->AddCapabilities(&caps);
  if (err != JVMTI_ERROR_NONE) {
    printf("Agent_OnLoad: Error in JVMTI AddCapabilities: %d\n", err);
  }

  err = jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks));
  if (err != JVMTI_ERROR_NONE) {
    printf("Agent_OnLoad: Error in JVMTI SetEventCallbacks: %d\n", err);
  }

  err = jvmti->CreateRawMonitor("Events Monitor", &event_mon);
  if (err != JVMTI_ERROR_NONE) {
    printf("Agent_OnLoad: Error in JVMTI CreateRawMonitor: %d\n", err);
  }

  printf("Agent_OnLoad finished\n");
  fflush(0);

  return JNI_OK;
}

JNIEXPORT void JNICALL
Java_MyPackage_ContStackDepthTest_enableEvents(JNIEnv *jni, jclass klass, jthread thread) {
  jint method_count = 0;
  jmethodID* methods = NULL;
  jmethodID method = NULL;
  jlocation location = (jlocation)0L;
  jvmtiError err;

  printf("enableEvents: started\n");
  exp_thread = (jthread)jni->NewGlobalRef(thread);

  err = jvmti->GetClassMethods(klass, &method_count, &methods);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI GetClassMethods");

  // Find jmethodID of fibTest()
  while (--method_count >= 0) {
    jmethodID meth = methods[method_count];
    char* mname = NULL;

    err = jvmti->GetMethodName(meth, &mname, NULL, NULL);
    check_jvmti_status(jni, err, "enableEvents: error in JVMTI GetMethodName call");

    if (strcmp(mname, "fibTest") == 0) {
      printf("enableEvents: found method fibTest() to set a breakpoint\n");
      fflush(0);
      method = meth;
    } 
  }
  if (method == NULL) {
    jni->FatalError("Error in enableEvents: not found method fibTest()");
  }

  err = jvmti->SetBreakpoint(method, location);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI SetBreakpoint");

  // Enable Breakpoint events globally
  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_BREAKPOINT, NULL);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI SetEventNotificationMode: enable BREAKPOINT");

  printf("enableEvents: finished\n");
  fflush(0);
}

JNIEXPORT jboolean JNICALL
Java_MyPackage_ContStackDepthTest_check(JNIEnv *jni, jclass cls) {
  jvmtiError err;

  printf("\n");
  printf("check: started\n");

  printf("check: breakpoint_count:   %d\n", breakpoint_count);
  printf("check: frame_pop_count:    %d\n", frame_pop_count);
  printf("check: method_entry_count: %d\n", method_entry_count);
  printf("check: method_exit_count:  %d\n", method_exit_count);
  printf("check: single_step_count:  %d\n", single_step_count);

  err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_METHOD_ENTRY, exp_thread);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI SetEventNotificationMode: disable METHOD_ENTRY");

  err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_METHOD_EXIT, exp_thread);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI SetEventNotificationMode: disable METHOD_EXIT");

  err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_FRAME_POP, exp_thread);
  check_jvmti_status(jni, err, "error in JVMTI SetEventNotificationMode: disable FRAME_POP");

  printf("check: finished\n");
  printf("\n");
  fflush(0);

  return (frame_pop_count == method_entry_count &&
          frame_pop_count == method_exit_count);
}
} // extern "C"
