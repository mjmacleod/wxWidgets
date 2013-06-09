/////////////////////////////////////////////////////////////////////////////
// Name:        src/unix/appunix.cpp
// Purpose:     wxAppConsole with wxMainLoop implementation
// Author:      Lukasz Michalski
// Created:     28/01/2005
// RCS-ID:      $Id$
// Copyright:   (c) Lukasz Michalski
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

#include "wx/wxprec.h"

#ifdef __BORLANDC__
    #pragma hdrstop
#endif

#ifndef WX_PRECOMP
    #include "wx/app.h"
    #include "wx/log.h"
#endif

#include "wx/evtloop.h"
#include "wx/unix/execute.h"
#include "wx/unix/private/wakeuppipe.h"
#include "wx/private/fdiodispatcher.h"
#include "wx/apptrait.h"
#include "wx/unix/apptbase.h"

#include <signal.h>
#include <unistd.h>
#include <errno.h>

#ifndef SA_RESTART
    // don't use for systems which don't define it (at least VMS and QNX)
    #define SA_RESTART 0
#endif

// ----------------------------------------------------------------------------
// Helper class calling CheckSignal() on wake up
// ----------------------------------------------------------------------------

namespace
{

class SignalsWakeUpPipe : public wxWakeUpPipe
{
public:
    SignalsWakeUpPipe() { }

    virtual void OnReadWaiting()
    {
        wxWakeUpPipe::OnReadWaiting();

        if ( wxTheApp )
            wxTheApp->CheckSignal();
    }
};

} // anonymous namespace

wxAppConsole::wxAppConsole()
{
    m_signalWakeUpPipe = NULL;
}

wxAppConsole::~wxAppConsole()
{
    delete m_signalWakeUpPipe;
}

// use unusual names for arg[cv] to avoid clashes with wxApp members with the
// same names
bool wxAppConsole::Initialize(int& argc_, wxChar** argv_)
{
    if ( !wxAppConsoleBase::Initialize(argc_, argv_) )
        return false;

    sigemptyset(&m_signalsCaught);

    return true;
}

// The actual signal handler. It does as little as possible (because very few
// things are safe to do from inside a signal handler) and just ensures that
// CheckSignal() will be called later from SignalsWakeUpPipe::OnReadWaiting().
void wxAppConsole::HandleSignal(int signal)
{
    wxAppConsole * const app = wxTheApp;
    if ( !app )
        return;

    // Register the signal that is caught.
    sigaddset(&(app->m_signalsCaught), signal);

    // Wake up the application for handling the signal.
    //
    // Notice that we must have a valid wake up pipe here as we only install
    // our signal handlers after allocating it.
    app->m_signalWakeUpPipe->WakeUpNoLock();
}

void wxOnReadWaiting(wxFDIOHandler* handler, int fd)
{
    if ( wxTheApp->GetTraits()->HasCallbackForFD(fd) )
        handler->OnReadWaiting();
}

void wxAppConsole::CheckSignal()
{
    for ( SignalHandlerHash::iterator it = m_signalHandlerHash.begin();
          it != m_signalHandlerHash.end();
          ++it )
    {
        int sig = it->first;
        if ( sigismember(&m_signalsCaught, sig) )
        {
            sigdelset(&m_signalsCaught, sig);
            (it->second)(sig);
        }
    }
}

bool wxAppConsole::RegisterSignalWakeUpPipe(wxFDIODispatcher& dispatcher)
{
    wxCHECK_MSG( m_signalWakeUpPipe, false, "Should be allocated" );

    return dispatcher.RegisterFD
                      (
                        m_signalWakeUpPipe->GetReadFd(),
                        m_signalWakeUpPipe,
                        wxFDIO_INPUT
                      );
}

// the type of the signal handlers we use is "void(*)(int)" while the real
// signal handlers are extern "C" and so have incompatible type and at least
// Sun CC warns about it, so use explicit casts to suppress these warnings as
// they should be harmless
extern "C"
{
    typedef void (*SignalHandler_t)(int);
}

bool wxAppConsole::SetSignalHandler(int signal, SignalHandler handler)
{
    const bool install = (SignalHandler_t)handler != SIG_DFL &&
                         (SignalHandler_t)handler != SIG_IGN;

    if ( !m_signalWakeUpPipe )
    {
        // Configure the pipe that the signal handler will use to
        // cause the event loop to call wxAppConsole::CheckSignal().
        m_signalWakeUpPipe = new SignalsWakeUpPipe();

        // Setup the callback for the wake-up pipe.
        GetTraits()->RegisterProcessCallback(*m_signalWakeUpPipe, m_signalWakeUpPipe->GetReadFd());
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (SignalHandler_t)&wxAppConsole::HandleSignal;
    sa.sa_flags = SA_RESTART;
    int res = sigaction(signal, &sa, 0);
    if ( res != 0 )
    {
        wxLogSysError(_("Failed to install signal handler"));
        return false;
    }

    if ( install )
        m_signalHandlerHash[signal] = handler;
    else
        m_signalHandlerHash.erase(signal);

    return true;
}

