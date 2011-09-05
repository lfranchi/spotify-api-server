#include <assert.h>
#include <jansson.h>
#include <libspotify/api.h>
#include <string.h>
#include <stdbool.h>

#include "constants.h"

json_t *playlist_to_json_set_collaborative(sp_playlist *playlist,
                                           json_t *object) {
  bool collaborative = sp_playlist_is_collaborative(playlist);
  json_object_set_new(object, "collaborative",
                      collaborative ? json_true() : json_false());
  return object;
}

json_t *playlist_to_json(sp_playlist *playlist, json_t *object) {
  assert(sp_playlist_is_loaded(playlist));

  // Owner
  sp_user *owner = sp_playlist_owner(playlist);
  const char *username = sp_user_display_name(owner);
  sp_user_release(owner);
  json_object_set_new_nocheck(object, "creator",
                              json_string_nocheck(username));

  // URI
  size_t playlist_uri_len = strlen("spotify:user:") + strlen(username) + 
                            strlen(":playlist:") +
                            strlen("284on3DVWeAxWkgVuzZKGt") + 1;
  char *playlist_uri = malloc(playlist_uri_len);

  if (playlist_uri == NULL) {
    return NULL;
  }

  sp_link *playlist_link = sp_link_create_from_playlist(playlist);
  sp_link_as_string(playlist_link, playlist_uri, playlist_uri_len);
  sp_link_release(playlist_link);
  json_object_set_new(object, "uri", 
                      json_string_nocheck(playlist_uri));
  free(playlist_uri);

  // Title
  const char *title = sp_playlist_name(playlist);
  json_object_set_new(object, "title",
                      json_string_nocheck(title));

  // Collaborative
  playlist_to_json_set_collaborative(playlist, object);

  // Description
  const char *description = sp_playlist_get_description(playlist);

  if (description != NULL) {
    json_object_set_new(object, "description",
                        json_string_nocheck(description));
  }

  // Number of subscribers
  int num_subscribers = sp_playlist_num_subscribers(playlist);
  json_object_set_new(object, "subscriberCount",
                      json_integer(num_subscribers));

  // Number of subscribers
  int num_tracks = sp_playlist_num_tracks(playlist);
  json_object_set_new(object, "trackCount",
                      json_integer(num_tracks));
                      
  // Tracks
  json_t *tracks = json_array();
  json_object_set_new(object, "tracks", tracks);
  char track_uri[kTrackLinkLength];
  
  for (int i = 0; i < sp_playlist_num_tracks(playlist); i++) {
    
   
    json_t *metadata = json_object(); // Create a new object
    json_t *artists = json_array(); // Track could have multiple artists
    sp_track *track = sp_playlist_track(playlist, i); 
    if(sp_track_is_loaded(track)){
	    sp_link *track_link = sp_link_create_from_track(track, 0);
	    sp_link_as_string(track_link, track_uri, kTrackLinkLength); // Could be nice to keep as ref?
	  
	    for(int j = 0; j < sp_track_num_artists(track); j++)
	   	json_array_append(artists, json_string_nocheck(sp_artist_name(sp_track_artist(track,j)))); // Append the artists
	    
	    // Whats needed and whats just some extra for future? 
	    if(sp_track_name(track))
	    	json_object_set_new_nocheck(metadata, "title", json_string_nocheck(sp_track_name(track)));
	    if(sp_album_name(sp_track_album(track)))	
	    	json_object_set_new_nocheck(metadata, "album", json_string_nocheck(sp_album_name(sp_track_album(track))));
	
	    json_object_set_new_nocheck(metadata, "trackuri", json_string_nocheck(track_uri));
	    
	    if(artists)
	    	json_object_set_new_nocheck(metadata, "artists", artists);
	    if(sp_track_duration(track))
	    	json_object_set_new_nocheck(metadata, "duration", json_integer(sp_track_duration(track)));
	    if(sp_track_popularity(track))	
	    	json_object_set_new_nocheck(metadata, "popularity", json_integer(sp_track_popularity(track)));
	    			    
	    json_array_append(tracks, metadata);
	    sp_link_release(track_link);
    }    
  }

  return object;
}

