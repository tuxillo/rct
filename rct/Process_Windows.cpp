/**
 * @file Process_Windows.cpp
 *
 * Containts the implementation for the Process class on Windows.
 */

#ifndef _WIN32
#  error "This file can only be built on Windows. On other OSs, build Process.cpp instead"
#endif

#include "Process.h"
#include "Log.h"

Process::Process()
    : mMode(Sync), mReturn(ReturnUnset)
{
    for(int i=0; i<NUM_HANDLES; i++)
    {
        mStdIn[i]  = mStdOut[i] = mStdErr[i] = INVALID_HANDLE_VALUE;
    }

    mProcess.dwProcessId = -1;
    mProcess.hThread = mProcess.hProcess = INVALID_HANDLE_VALUE;
}

Process::~Process()
{
    waitForProcessToFinish();

    for(int i=0; i<NUM_HANDLES; i++)
    {
        closeHandleIfValid(mStdIn[i]);
        closeHandleIfValid(mStdOut[i]);
        closeHandleIfValid(mStdErr[i]);
    }

    if(mthStdout.joinable()) mthStdout.join();
    if(mthStderr.joinable()) mthStderr.join();
}

/*static*/ void Process::closeHandleIfValid(HANDLE &f_handle)
{
    if(f_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(f_handle);
    }
    f_handle = INVALID_HANDLE_VALUE;
}

Process::ExecState Process::exec(const Path &f_cmd,
                                 const List<String> &f_args,
                                 int f_timeout_ms, unsigned int f_flags)
{
    mMode = Sync;
    auto ret = startInternal(f_cmd, f_args, List<String>(), f_timeout_ms, f_flags);

    if(mthStdout.joinable()) mthStdout.join();
    if(mthStderr.joinable()) mthStderr.join();

    return ret;
}

bool Process::start(const Path &f_cmd,
                    const List<String> &f_args,
                    const List<String> &f_environ)
{
    mMode = Async;
    return startInternal(f_cmd, f_args, f_environ) == Done;
}

Process::ExecState Process::startInternal(const Path &f_cmd, const List<String> &f_args,
                            const List<String> &f_environ, int f_timeout_ms,
                            unsigned int f_flags)
{
    // unused arguments (for now)
    (void) f_args;
    (void) f_environ;
    (void) f_timeout_ms;
    (void) f_flags;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(saAttr);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    // Create anonymous pipes which we will use
    if(!CreatePipe(&mStdIn [READ_END], &mStdIn [WRITE_END], &saAttr, 0) ||
       !CreatePipe(&mStdOut[READ_END], &mStdOut[WRITE_END], &saAttr, 0) ||
       !CreatePipe(&mStdErr[READ_END], &mStdErr[WRITE_END], &saAttr, 0))
    {
        error() << "Error creating pipes";
        return Error;
    }

    // the child is not supposed to gain access to the pipes' parent end
    if(!SetHandleInformation(mStdIn[WRITE_END], HANDLE_FLAG_INHERIT, 0) ||
       !SetHandleInformation(mStdOut[READ_END], HANDLE_FLAG_INHERIT, 0) ||
       !SetHandleInformation(mStdErr[READ_END], HANDLE_FLAG_INHERIT, 0))
    {
        error() << "SetHandleInformation: " <<
            static_cast<long long int>(GetLastError());
        return Error;
    }

    // set up STARTUPINFO structure. It tells CreateProcess to use the pipes
    // we just created as stdin, stdout and stderr for the new process.
    STARTUPINFO siStartInfo;
    memset(&siStartInfo, 0, sizeof(siStartInfo));
    siStartInfo.cb = sizeof(siStartInfo);
    siStartInfo.hStdInput  = mStdIn[READ_END];
    siStartInfo.hStdOutput = mStdOut[WRITE_END];
    siStartInfo.hStdError  = mStdErr[WRITE_END];
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    memset(&mProcess, 0, sizeof(mProcess));

    // CreateProcess takes a non-const pointer as the command, so we need
    // to create a copy.
    std::string nonConstCommand(f_cmd);

    if(!CreateProcess(NULL,  // application name: we pass it through lpCommandLine
                      &nonConstCommand[0],
                      NULL,  // security attrs
                      NULL,  // thread security attrs
                      TRUE,  // handles are inherited
                      0,     // creation flags
                      NULL,  // TODO: env
                      NULL,  // TODO: cwd
                      &siStartInfo,  // in: stdin, stdout, stderr pipes
                      &mProcess    // out: info about the new process
                             ))
    {
        error() << "Error in CreateProcess(): "
                << static_cast<long long int>(GetLastError());
        return Error;
    }

    // we need to close our handles to the write end of these pipe. Otherwise,
    // ReadFile() will not return when the child process terminates.
    closeHandleIfValid(mStdOut[WRITE_END]);
    closeHandleIfValid(mStdErr[WRITE_END]);

    // We start one thread for each pipe (child process' stdout and child process'
    // stderr). We could use overlapping IO here, but it's very complicated and
    // probably doesn't do what we want exactly, so we stick with the (somewhat
    // ugly) two-thread solution.

    mthStdout = std::thread(&Process::readFromPipe, this, mStdOut[READ_END],
                            std::ref(mReadyReadStdOut),
                  std::bind(&Process::waitForProcessToFinish, this));

    mthStderr = std::thread(&Process::readFromPipe, this, mStdErr[READ_END],
                            std::ref(mReadyReadStdErr),
                            nullptr);

    return Done;
}

void Process::readFromPipe(HANDLE f_pipe,
                           Signal<std::function<void(Process*)> > &f_signalGotSth,
                           std::function<void ()> f_execAfter)
{
    (void) f_pipe;

    CHAR buf[PIPE_READ_BUFFER_SIZE];
    DWORD bytesRead = 0;

    bool moreToRead = true;

    while(moreToRead)
    {
        if(ReadFile(mStdOut[READ_END], buf,
                    PIPE_READ_BUFFER_SIZE, &bytesRead, NULL))
        {
            std::lock_guard<std::mutex> lo(mMutex);
            mStdOutBuffer.append(buf, bytesRead);
            (f_signalGotSth)(this);
        }
        else
        {
            DWORD err = GetLastError();

            if(err == ERROR_BROKEN_PIPE)
            {
                // child process terminated (this is not an error)
            }
            else
            {
                error() << "Error while reading from child process: "
                        << static_cast<long long int>(err) << "\n";
            }

            moreToRead = false;
        }
    }

    if(f_execAfter) f_execAfter();
}

void Process::waitForProcessToFinish()
{
    if(mProcess.hProcess == INVALID_HANDLE_VALUE) return;  // already finished.

    DWORD res = WaitForSingleObject(mProcess.hProcess, INFINITE);
    if(res != WAIT_OBJECT_0)
    {
        error() << "Error waiting for process to finish: "
                << static_cast<long long int>(res) << "\n";
    }

    // store exit code
    DWORD retCode;
    GetExitCodeProcess(mProcess.hProcess, &retCode);
    mReturn = retCode;

    // send 'finished' signal
    mFinished(this);

    // Close remaining handles so that the OS can clean up
    closeHandleIfValid(mProcess.hThread);
    closeHandleIfValid(mProcess.hProcess);
}

int Process::returnCode() const
{
     std::lock_guard<std::mutex> lock(mMutex);
     return mReturn;
}

bool Process::isFinished() const
{
    return mProcess.hProcess == INVALID_HANDLE_VALUE;
}