#include <memory>
#include <iostream>

#include <jvmti.h>

static jvmtiEnv *jvmti;

// TODO: the name can be parsed from cmdline
#define THREAD_TO_CATCH "HighResTimer"

template <typename T>
class Holder
{
public:
	Holder() = default;
	explicit Holder(T *ptr) noexcept
			: ptr_(ptr)
	{
	}

	Holder(const Holder &) = delete;
	Holder &operator=(const Holder &other) = delete;

	Holder(Holder &&other) noexcept
	{
		deallocate();
		ptr_ = nullptr;
		std::swap(ptr_, other.ptr_);
	}

	Holder &operator=(Holder &&other) = default;

	~Holder()
	{
		deallocate();
	}

	bool valid() const
	{
		return ptr_ != nullptr;
	}

	operator bool() const
	{
		return valid();
	}

	T **ptr()
	{
		return &ptr_;
	}

	const T *get() const
	{
		return ptr_;
	}

private:
	void deallocate() noexcept
	{
		if (valid())
		{
			jvmti->Deallocate((unsigned char *)ptr_);
		}
	}

private:
	T *ptr_{nullptr};
};

using JNIChar = Holder<char>;
using JNIStackInfo = Holder<jvmtiStackInfo>;

JNIChar
getClassSignature(jclass cls)
{
	char *signature; // deallocate
	if (jvmti->GetClassSignature(cls, &signature, nullptr) != JNI_OK)
	{
		std::cerr << "failed to take class signature";
		return JNIChar(nullptr);
	}
	return JNIChar(signature);
}

JNIChar
getMethodName(jmethodID method)
{
	char *name;
	if (jvmti->GetMethodName(method, &name, NULL, NULL) != JNI_OK)
	{
		std::cerr << "failed to take class signature";
		return JNIChar(nullptr);
	}

	return JNIChar(name);
}

void printStackTrace(jthread thread)
{
	jvmtiThreadInfo info;
	if (jvmti->GetThreadInfo(thread, &info) == JNI_OK)
	{
		std::cerr << "========= " << info.name << " ==============\n";
	}

	#define MAX_FRAMES 10
	jvmtiFrameInfo frames[MAX_FRAMES];
	jint count;
	jvmtiError err;
	// Look at Thread implementatio
	// skip 2 frames of init overloads
	if (jvmti->GetStackTrace(thread, 2, MAX_FRAMES, frames, &count) != JNI_OK)
	{
		std::cerr << "GetStackTrace failed\n";
		return;
	}

	for (auto i = 0; i < count; i++)
	{
		jclass cls; // check if we need to deallocate that
		if (jvmti->GetMethodDeclaringClass(frames[i].method, &cls) != JNI_OK)
		{
			std::cerr << "frame skip. Failed to get classID\n";
		}
		auto className = getClassSignature(cls);
		auto methodName = getMethodName(frames[i].method);
		std::cerr << className.get() << "#" << methodName.get() << "\n";
	}
}

static jmethodID thread_start{0};
static jfieldID thread_name_field{0};

extern "C"
{

	void
	ClassLoad(jvmtiEnv *jvmti_env,
						JNIEnv *jni,
						jthread thread,
						jclass klass)
	{
		auto name = getClassSignature(klass);
		std::string s(name.get());
		if (s == "Ljava/lang/Thread;")
		{
			thread_start = jni->GetMethodID(klass, "start", "()V");
			thread_name_field = jni->GetFieldID(klass, "name", "Ljava/lang/String;");
			if (jvmti->SetFieldModificationWatch(klass, thread_name_field) != JNI_OK)
			{
				std::cerr << "failed to attach field watcher\n";
			}
		}
	}

	void
	OnFieldModification(jvmtiEnv *jvmti,
											JNIEnv *jni,
											jthread thread,
											jmethodID method,
											jlocation location,
											jclass field_klass,
											jobject object,
											jfieldID field,
											char signature_type,
											jvalue new_value)
	{
		if (field != thread_name_field)
		{
			return;
		}
		const char* name = jni->GetStringUTFChars((jstring)new_value.l, NULL);
		if (strcmp(name, THREAD_TO_CATCH) != 0)
		{
			jni->ReleaseStringUTFChars((jstring)new_value.l, name);
			return;
		}
		std::cerr << "Thread " << name << " is about to get started\n";
		jni->ReleaseStringUTFChars((jstring)new_value.l, name);
		printStackTrace(thread);
	}

	JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved)
	{
		if (jvm->GetEnv((void **)&jvmti, JVMTI_VERSION_1_2) != JNI_OK)
		{
			std::cerr << "Unable to access jvmti\n";
			return JNI_ERR;
		}

		jvmtiCapabilities capabilities = {0};
		capabilities.can_generate_all_class_hook_events = 1;
		capabilities.can_retransform_classes = 1;
		capabilities.can_retransform_any_class = 1;
		capabilities.can_get_bytecodes = 1;
		capabilities.can_get_constant_pool = 1;
		capabilities.can_get_source_file_name = 1;
		capabilities.can_get_line_numbers = 1;
		capabilities.can_generate_compiled_method_load_events = 1;
		capabilities.can_generate_monitor_events = 1;
		capabilities.can_generate_method_entry_events = 1;
		capabilities.can_tag_objects = 1;
		capabilities.can_generate_field_modification_events = 1;
		if (jvmti->AddCapabilities(&capabilities) != JNI_OK)
		{
			std::cerr << "AddCapabilities failed\n";
			return JNI_ERR;
		}

		jvmtiEventCallbacks callbacks = {0};
		callbacks.ClassLoad = ClassLoad;
		callbacks.FieldModification = OnFieldModification;
		if (jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JNI_OK)
		{
			std::cerr << "SetEventCallbacks failed\n";
			return JNI_ERR;
		}

		if (jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_START, NULL) != JNI_OK)
		{
			std::cerr << "SetEventNotificationMode JVMTI_EVENT_VM_START failed\n";
			return JNI_ERR;
		}
		if (jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_LOAD, NULL) != JNI_OK)
		{
			std::cerr << "SetEventNotificationMode JVMTI_EVENT_CLASS_LOAD failed\n";
			return JNI_ERR;
		}
		if (jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_FIELD_MODIFICATION, NULL) != JNI_OK)
		{
			std::cerr << "SetEventNotificationMode JVMTI_EVENT_FIELD_MODIFICATION failed\n";
			return JNI_ERR;
		}

		std::cerr << "Agent loaded successfully\n";
		return JNI_OK;
	}
}
