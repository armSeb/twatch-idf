# Démo — « Tír na nÓg »

**Français** · [English](../en/demo.md)

Une intro façon démoscène d'environ 2 minutes, entièrement calculée par la montre : pas une
seule image ni un seul son stocké. Étoiles, textures, objets 3D, paysage et musique sont
calculés à chaque image. Le nom est celui de l'autre monde de la mythologie irlandaise, la
« terre de la jeunesse ».

![Démo](../img/demo-fr.png)

## Déroulé

| Moment | Scène | Musique |
|---|---|---|
| 0:00 | champ d'étoiles qui accélère, « Seb & Claude présentent », ~900 particules forment le titre, explosion | arpège seul |
| 0:08 | tunnel infini texturé (entrelacs), « 100 % calculé sur la montre » | basse, charleston |
| 0:15 | rotozoom de l'entrelacs sur un plasma | couplet |
| 0:23 | tore 3D ombré qui gonfle sur la grosse caisse | |
| 0:31 | nuée de 700 points qui devient sphère, tore, cube puis galaxie, devant des barres « copper » | refrain |
| 0:46 | métaballes et générique | pont, nappe |
| 0:54 | le titre brûle, le feu monte avec le roulement | montée |
| 1:01 | survol de l'Irlande au coucher du soleil (voir plus bas), défilement des salutations | couplet, refrain |
| 1:32 | nœud celtique en 3D | final, mélodie à l'octave |
| 1:48 | retour aux étoiles, « MERCI ! », fondu | fin |

À la fin : **toucher l'écran** relance la démo. Pendant la démo, toucher **coupe ou remet le
son** (le morceau continue en silence, l'image reste calée). **Glisser** pour quitter.

L'écran reste allumé et le processeur à 240 MHz tant que la démo est affichée ; tout s'arrête
dès qu'on la quitte ou qu'on éteint l'écran avec le bouton.

## Le paysage

Inventé à chaque lancement (il est différent à chaque fois), en tâche de fond pendant les
premières scènes : relief à plusieurs octaves avec massifs enneigés, rivières qui serpentent,
lacs et plages, prés en patchwork bordés de haies, champs labourés et de foin, forêts,
fermettes blanches, landes et roche. Il est éclairé par un soleil bas avec **ombres portées**,
la brume monte vers l'horizon, les nuages défilent en perspective, l'eau reflète le ciel et
miroite. Le pilote automatique suit les vallées. Le même paysage sert au jeu **Envol**
([guide](flight.md)).

## La musique

Un petit synthétiseur façon tracker (`demo_synth.c`), 125 BPM, en la mineur : 64 mesures
(couplet la m – fa – do – sol, refrain fa – sol – mi m – la m, pont ré m – la m – fa – sol).
Mélodie en carré 25 % avec un écho, arpège et basse en carré 50 %, nappe de trois triangles
légèrement désaccordés, grosse caisse en sinus qui plonge, caisse claire et charleston au
bruit. Rendu à 16 kHz dans sa propre tâche ; c'est aussi l'horloge de l'image.

## Comment c'est fait

L'image est calculée en 120×120 dans un tampon en RAM interne, puis doublée dans un canevas
240×240 en PSRAM que LVGL envoie à l'écran.

- **Tunnel** : tables précalculées d'angle, de profondeur et d'ombre par pixel ; chaque image
  ne fait qu'une lecture de texture et une palette qui tourne.
- **Rotozoom** : texture parcourue en virgule fixe le long d'une droite tournée, mélangée à
  un plasma par tables de sinus.
- **Tore et nœud** : maillages tournés sur trois axes, faces cachées éliminées, éclairage par
  face (diffus + spéculaire), tri du plus loin au plus proche, triangles remplis par lignes.
  Le nœud est un tube de 48 × 8 facettes autour d'un nœud de trèfle.
- **Points** : 700 points interpolés entre deux formes, lueurs additives.
- **Métaballes** : champ de cinq sources calculé en 60×60, isolignes dorées.
- **Feu** : automate classique (moyenne des voisins du dessous), le texte sert de foyer.
- **Paysage** : « voxel space » colonne par colonne, hauteurs et couleurs lissées au premier
  plan — voir [l'architecture](architecture.md).
- **Texte** : petite police 5×7 maison, accents dessinés.
