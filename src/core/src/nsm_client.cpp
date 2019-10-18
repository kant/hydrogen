/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 *
 * http://www.hydrogen-music.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include "hydrogen/helpers/filesystem.h"
#include "hydrogen/Preferences.h"

#include <pthread.h>
#include <unistd.h>

#if defined(H2CORE_HAVE_OSC) || _DOXYGEN_

#include "hydrogen/nsm_client.h"
#include "hydrogen/event_queue.h"
#include "hydrogen/hydrogen.h"
#include "hydrogen/basics/song.h"


NsmClient * NsmClient::__instance = nullptr;
const char* NsmClient::__class_name = "NsmClient";
bool NsmShutdown = false;


static int nsm_open_cb (const char *name,
						const char *display_name,
						const char *client_id,
						char **out_msg,
						void *userdata )
{
	MidiActionManager* pActionManager = MidiActionManager::get_instance();
	
	// Handle supplied Song name. Hydrogen sends a unique string, like
	// - if the display_name == Hydrogen - "Hydrogen.nJKUV". We will
	// just append the .h2song and use it as Song path.
	
	if ( !name ) {
		___ERRORLOG( QString( "No `name` provided!" ) );
		return ERR_LAUNCH_FAILED;
	}

	QString songPath = QString( "%1.h2song" ).arg( name );
	QFileInfo songFileInfo = QFileInfo( songPath );
	
	H2Core::Hydrogen *pHydrogen = H2Core::Hydrogen::get_instance();

	// When starting Hydrogen with its Qt5 GUI activated the chosen
	// Song will be set via the GUI. But since it is constructed after
	// the NSM client, using the corresponding OSC message to open a
	// Song won't work in this scenario (since this would set the Song
	// asynchronously using the EventQueue and it is require during
	// the construction of MainForm). Therefore the next line will
	// check whether there is a GUI present and if it is setup
	// already.
	if ( pHydrogen->getActiveGUI() < 0 ) {
		
		// There is a GUI, but it is not ready yet. Therefore we have
		// to duplicate some of the logic from the OSC messages,
		// create a new Song object, and store it for the GUI to load
		// later on.
		if ( songFileInfo.exists() ) {
			
			H2Core::Song *pSong = H2Core::Song::load( songPath );
	
			if ( pSong == nullptr ) {
				___ERRORLOG( QString( "Error: Unable to open Song." ) );
		
				return ERR_LAUNCH_FAILED;
			}
			
			pHydrogen->setNextSong( pSong );
			
		} else {
			
			// There is no file with this name present yet. Fall back
			// to an empty default Song.
			H2Core::Song *pSong = H2Core::Song::get_empty_song();
			pSong->set_filename( songPath );
			
			pHydrogen->setNextSong( pSong );
			
		}
	} else {
		
		// The opening of the Song will be done (asynchronously, if a
		// GUI is present) using the MidiActionManager.
	
		// Check whether a file corresponding to the provided path does
		// already exist.
		QString actionType;
		if ( songFileInfo.exists() ) {
			// Open the existing file.
			actionType = "OPEN_SONG";
		} else {
			// Create a new file and save it as using the provided path.
			actionType = "NEW_SONG";
		}

		Action currentAction( actionType );
		
		// Tell the action to load the desired file.
		currentAction.setParameter1( songPath );
		
		// Tell the action to restart the audio engine.
		currentAction.setParameter2( "1" );
		bool ok = pActionManager->handleAction( &currentAction );

		if ( !ok ) {
			___ERRORLOG( QString( "Unable to handle opening action!" ) );
			return ERR_LAUNCH_FAILED;
		}
		
	}

	// Restarting the JACK server using the client_id in the name of
	// the freshly created instance.
	H2Core::Preferences *pPref = H2Core::Preferences::get_instance();

	if ( pPref ){
		if ( client_id ){
			// Setup JACK here, client_id gets the JACK client name
			pPref->setNsmClientId( QString( client_id ) );
		} else {
			___ERRORLOG( "No client_id supplied in NSM open callback!" );
			return ERR_LAUNCH_FAILED;
		}
	} else {
		
		___ERRORLOG( "Preferences instance is not ready yet!" );
		return ERR_NOT_NOW;
		
	}
	
	return ERR_OK;
}

static int nsm_save_cb( char **out_msg, void *userdata )
{
	Action currentAction("SAVE_SONG");
	MidiActionManager* pActionManager = MidiActionManager::get_instance();

	pActionManager->handleAction(&currentAction);

	return ERR_OK;
}

void* nsm_processEvent(void* data)
{
	nsm_client_t* nsm = (nsm_client_t*) data;

	while(!NsmShutdown && nsm){
		nsm_check_wait( nsm, 1000);
	}

	return nullptr;
}

NsmClient::NsmClient()
	: Object( __class_name )
{
	m_NsmThread = 0;
	m_bUnderSessionManagement = false;
}

void NsmClient::create_instance()
{
	if( __instance == nullptr ) {
		__instance = new NsmClient;
	}
}

NsmClient::~NsmClient()
{
	__instance = nullptr;
}

void NsmClient::shutdown()
{
	NsmShutdown = true;
}

void NsmClient::createInitialClient()
{
	/*
	 * Make first contact with NSM server.
	 */

	nsm_client_t* nsm = nullptr;

	H2Core::Preferences *pPref = H2Core::Preferences::get_instance();
	QString H2ProcessName = pPref->getH2ProcessName();
	QByteArray byteArray = H2ProcessName.toLatin1();

	const char *nsm_url = getenv( "NSM_URL" );

	if ( nsm_url )
	{
		nsm = nsm_new();
		
		// Store the nsm client in a private member variable for later
		// access.
		m_nsm = nsm;

		if ( nsm )
		{
			nsm_set_open_callback( nsm, nsm_open_cb, (void*) nullptr );
			nsm_set_save_callback( nsm, nsm_save_cb, (void*) nullptr );

			if ( nsm_init( nsm, nsm_url ) == 0 )
			{
				nsm_send_announce( nsm, "Hydrogen", ":dirty:switch:", byteArray.data() );
				nsm_check_wait( nsm, 10000 );

				if(pthread_create(&m_NsmThread, nullptr, nsm_processEvent, nsm)) {
					___ERRORLOG("Error creating NSM thread\n	");
					return;
				}
				
				// Everything worked fine and H2 can now be considered
				// under session management.
				m_bUnderSessionManagement = true;

			}
			else
			{
				___ERRORLOG("failed, freeing NSM client");
				nsm_free( nsm );
				nsm = nullptr;
			}
		}
	}
	else
	{
		___WARNINGLOG("No NSM URL available: no NSM management\n");
	}
}

void NsmClient::sendDirtyState( const bool isDirty ) {
	
	if ( isDirty ) {
		nsm_send_is_dirty( m_nsm );
	} else {
		nsm_send_is_clean( m_nsm );
	}
}

#endif /* H2CORE_HAVE_OSC */

