// app/include/env.h
#ifndef ENV_H
#define ENV_H

// Toujours commenter : endpoint remote (backend) pour ingest des batchs ACAP
// ⚠️ ne pas commit une URL interne si elle est sensible
#define HTTP_TARGET "https://api.mycarcounter.fr/api/enregistrements/ingest-batch-from-acap"

// Toujours commenter : Auth0 token endpoint + paramètres client_credentials
// ⚠️ idéalement : ne pas commiter AUTH0_CLIENT_SECRET (mettre dans un env.h local gitignored)
#define AUTH0_URL "https://dev-6xrnn215zh26wxwo.us.auth0.com/oauth/token"
#define AUTH0_CLIENT_ID "vtJWEop6rfpRz59PoFmfni27Fe7iwI4Z"
#define AUTH0_CLIENT_SECRET "W04lUw4exH82IV4CWPd3i7ObsYAIAlWYINicuJHTlHZuRseoLZvhUl8f0YIQwcCn"
#define AUTH0_AUDIENCE "https://api.mycarcounter.fr"
#define AUTH0_GRANT_TYPE "client_credentials"

// Toujours commenter : credentials caméra (DigestAuth / contrôle.cgi)
// ⚠️ idéalement : ne pas commiter PASS (mettre dans un env.h local gitignored)
#define USER "root"
#define PASS "#Ch@mpIe1nMyCC"

// Toujours commenter : legacy (si encore utilisé) - scénarios “en dur”
// NOTE : à terme vous tickez sur tous les scénarios valides listés via getConfiguration.
#define NUM_SCENARIO_DIR1_OU_DVG "1"
#define NUM_SCENARIO_DIR2_OU_GVD "2"

#endif // ENV_H
