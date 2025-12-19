// #pragma once

// Paramètres :
// - ip : IP/hostname de la caméra (souvent "127.0.0.1").
// - user/pass : identifiants Digest pour appeler control.cgi.
void test_camera_req_list_scenarios(const char *ip, const char *user, const char *pass);
