/******************************************************************************
* H2026
* LABORATOIRE 4, Systèmes embarqués et temps réel
* Ébauche de code pour le pilote utilisant les interruptions
* Marc-André Gardner, H2026
*
* Ce fichier contient la structure du pilote qu'il vous faut implémenter. Ce
* pilote fonctionne avec des interruptions, c'est-à-dire qu'il vérifie la valeur
* des touches appuyées que lorsqu'une touche est effectivement enfoncée.
*
* Prenez le temps de lire attentivement les notes de cours et les commentaires
* contenus dans ce fichier, ils contiennent des informations cruciales.
*
*/

// Inclusion des en-têtes nécessaires
// Vous pouvez en ajouter, mais n'oubliez pas que vous n'avez PAS
// accès la libc! Vous ne pouvez vous servir que des fonctions fournies
// par le noyau Linux.
#include <linux/init.h>             // Macros spécifiques des fonctions d'un module
#include <linux/module.h>           // En-tête général des modules noyau
#include <linux/device.h>           // Pour créer un pilote
#include <linux/kernel.h>           // Différentes définitions de types liés au noyau
#include <linux/gpio.h>             // Pour accéder aux GPIO du Raspberry Pi
#include <linux/gpio/machine.h>     // Idem
#include <linux/fs.h>               // Pour accéder au système de fichier et créer un fichier spécial dans /dev
#include <linux/uaccess.h>          // Permet d'accéder à copy_to_user et copy_from_user
#include <linux/delay.h>            // Fonctions d'attente, en particulier msleep
#include <linux/string.h>           // Différentes fonctions de manipulation de string, plus memset et memcpy
#include <linux/mutex.h>            // Mutex et synchronisation
#include <linux/interrupt.h>        // Définit les symboles pour les interruptions et les tasklets
#include <linux/atomic.h>           // Synchronisation par valeur atomique

// Le nom de notre périphérique et le nom de sa classe
#define DEV_NAME "setrclavier"
#define CLS_NAME "setr"

// Le nombre de caractères pouvant être contenus dans le buffer circulaire
// Ce nombre est volontairement très bas pour vous permettre de tester votre logique
// de buffer circulaire en cas de dépassement de la capacité du buffer
#define TAILLE_BUFFER 10

// Définit le nombre de lignes et de colonnes de votre clavier
// TODO: adaptez-le selon le modèle de clavier que vous avez!
#define NOMBRE_LIGNES 4
#define NOMBRE_COLONNES 3


// On déclare tout de suite le nom de la fonction gérant les interruptions
static irqreturn_t  setr_irq_handler(unsigned int irq, void *dev_id);

// Déclaration des fonctions pour gérer notre fichier
// Nous ne définissons que open(), close() et read()
static int     dev_open(struct inode *, struct file *);
static int     dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);

static struct file_operations fops =
{
   .open = dev_open,
   .read = dev_read,
   .release = dev_release,
};

// Variables globales et statiques utilisées dans le driver
static int    majorNumber;                  // Numéro donné par le noyau à notre pilote
static char   data[TAILLE_BUFFER] = {0};    // Buffer circulaire contenant les caractères du clavier
static size_t posCouranteLecture = 0;       // Position de la prochaine lecture dans le buffer
static size_t posCouranteEcriture = 0;      // Position de la prochaine écriture dans le buffer

static struct class*  setrClasse  = NULL;   // Contiendra les informations sur la classe de notre pilote
static struct device* setrDevice = NULL;    // Contiendra les informations sur le périphérique associé

static struct mutex sync;                   // Mutex servant à synchroniser les accès au buffer
static atomic_t irqEnCours = ATOMIC_INIT(0);  // Pour déterminer si les interruptions doivent être traitées


// 4 GPIO doivent être assignés pour l'écriture, et 3 ou 4 en lecture (voir énoncé)
// Nous vous proposons les choix suivants, mais ce n'est pas obligatoire
// Dans la table suivante, chaque ligne réfère à _un_ GPIO en particulier.
// Le premier argument de chaque ligne ("pinctrl-bcm2835") ne doit pas être changé,
//      il réfère au contrôleur enregistré sur le Raspberry Pi Zero W.
// Le second argument est le numéro du _GPIO_ (PAS le Pin# du Raspberry Pi). Par
//      exemple, la _pin_ 36 du Raspberry Pi Zero correspond au GPIO 16, c'est donc
//      16 qu'il faut mettre ici. Voyez le schéma au début de l'énoncé pour plus de détails.
// Le 3e argument est un identifiant que nous utiliserons pour enregistrer nos GPIO en groupe
//      (tous les GPIO d'écriture ensemble et tous les GPIO de lecture ensemble) avec
//      gpiod_get_array.
// Le 4e argument est un index qui désigne spécifiquement chaque GPIO dans un groupe. Lorsque
//      vous lirez ou écrirez dans un groupe, le "bitmap" demandé est un entier où chaque _bit_
//      correspond à l'état d'un GPIO. Par exemple, avec la table telle que définie plus bas,
//      le bit le moins significatif (LSB) du bitmap appliqué sur le groupe "écriture"
//      écrira/lira le GPIO 5. Le second bit sera lié au GPIO 6, le 3e au GPIO 13, et ainsi de suite.
// Le 5e argument est une suite de drapeaux paramétrant le GPIO. Ici, nous n'avons besoin que 
//      de signifier que nous voulons que nos GPIO soient "active high", c'est-à-dire qu'écrire un
//      "1" entraîne un voltage "haut" sur la pin. Vous pouvez trouver les autres drapeaux possibles
//      ici : https://www.kernel.org/doc/html/v5.2/driver-api/gpio/board.html#platform-data
static  struct gpiod_lookup_table gpios_table = {
    .dev_id = DEV_NAME,
    .table = {           /* Nom du contrôleur,GPIO, identifiant, index, actif haut ou bas */
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 5,  "ecriture", 0, GPIO_ACTIVE_HIGH),
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 6,  "ecriture", 1, GPIO_ACTIVE_HIGH),
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 13, "ecriture", 2, GPIO_ACTIVE_HIGH),
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 19, "ecriture", 3, GPIO_ACTIVE_HIGH),
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 12, "lecture",  0, GPIO_ACTIVE_HIGH),
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 16, "lecture",  1, GPIO_ACTIVE_HIGH),
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 20, "lecture",  2, GPIO_ACTIVE_HIGH),
            #if NOMBRE_COLONNES == 4
            GPIO_LOOKUP_IDX("pinctrl-bcm2835", 21, "lecture", 3, GPIO_ACTIVE_HIGH),
            #endif
            { },        // Toujours laisser une entrée vide à la fin!
    },
};

// Contiendra les descripteurs conservant la configuration des GPIO
static struct gpio_descs *gpioLecture, *gpioEcriture;

    
// Les valeurs du clavier, selon la ligne et la colonne actives
#if NOMBRE_COLONNES == 3
    static char valeursClavier[NOMBRE_LIGNES][NOMBRE_COLONNES] = {
        {'1', '2', '3'},
        {'4', '5', '6'},
        {'7', '8', '9'},
        {'*', '0', '#'}
    };
#else
    static char valeursClavier[NOMBRE_LIGNES][NOMBRE_COLONNES] = {
        {'1', '2', '3', 'A'},
        {'4', '5', '6', 'B'},
        {'7', '8', '9', 'C'},
        {'*', '0', '#', 'D'}
    };
#endif

// Permet de se souvenir du dernier état du clavier,
// pour ne pas répéter une touche qui était déjà enfoncée.
static int dernierEtat[NOMBRE_LIGNES][NOMBRE_COLONNES] = {0};

// Contient les numéros d'interruption pour chaque broche de lecture
static unsigned int irqId[NOMBRE_COLONNES];               



void func_tasklet_polling(unsigned long paramf){
    // TODO
    // Déclarez _toutes_ vos variables locales ici (le module est compilé avec un standard générant
    // un warning si une variable est déclarée après toute ligne de code)
    
    // Cette fonction est le coeur d'exécution du tasklet
    // Elle fait à peu de choses près la même chose que le kthread
    // dans le pilote que vous avez précédemment écrit (par polling),
    // à savoir qu'elle balaye les différentes lignes pour trouver quelle
    // touche est pressée.
    // Une différence majeure est que ce tasklet ne s'exécute pas sans cesse,
    // il ne s'exécute qu'une seule fois par interruption!
    //
    // Important: Vous DEVEZ utiliser l'API "GPIO Descriptor Consumer Interface"
    // (ref : https://www.kernel.org/doc/html/v6.1/driver-api/gpio/consumer.html)
    

    // TODO
    // Écrivez le code permettant
    // 1) D'éviter le traitement de nouvelles interruptions : nous allons changer
    //      les niveaux des broches de lecture, il ne faut pas que ce soit interprété
    //      comme une nouvelle pression sur une touche, sinon ce tasklet sera rappelé
    //      en boucle! Vous êtes libres d'utiliser l'approche que vous souhaitez pour
    //      éviter cela, mais la variable atomique irqEnCours pourrait vous être utile...
    // 2) De passer au travers de tous les patrons de balayage
    // 3) Pour chaque patron, vérifier la valeur des lignes d'entrée
    // 4) Selon ces valeurs et le contenu de dernierEtat, déterminer si une nouvelle touche a été pressée
    // 5) Mettre à jour le buffer et dernierEtat en vous assurant d'éviter les race conditions avec le reste du module
    // 6) Remettre toutes les lignes à 1 (pour réarmer l'interruption)
    // 7) Réactiver le traitement des interruptions
    //
    // Information importante : vous n'en avez techniquement pas besoin de toute façon puisque
    // cette fonction n'a pas à être exécutée en boucle, mais vous ne pouvez _pas_
    // faire un msleep ou une autre fonction similaire dans un tasklet!

}

// On déclare le tasklet avec la macro DECLARE_TASKLET_OLD
DECLARE_TASKLET_OLD(tasklet_polling, func_tasklet_polling);


static irqreturn_t  setr_irq_handler(unsigned int irq, void *dev_id){
    // Ceci est la fonction recevant l'interruption. Son seul rôle consiste à
    // céduler un tasklet qui fera le travail effectif de balayage.
    // Attention toutefois : ce balayage ne doit pas faire en sorte que de _nouvelles_
    // interruptions soient traitées et lancent encore le tasklet, sinon vous vous
    // retrouverez dans une boucle sans fin où le tasklet crée des interruptions,
    // qui lancent le tasklet, qui crée des interruptions, etc.
    // Voyez les commentaires du tasklet pour une piste potentielle de synchronisation.
    //
    // N'oubliez pas que ce IRQ handler devrait en faire le _minimum_ et déférer le
    // plus possible le traitement au tasklet!
    // TODO

    // On retourne en indiquant qu'on a géré l'interruption
    return (irqreturn_t) IRQ_HANDLED;
}


static int __init setrclavier_init(void){
    // TODO
    // Déclarez _toutes_ vos variables locales ici (le module est compilé avec un standard générant
    // un warning si une variable est déclarée après toute ligne de code)
    int ok;
    printk(KERN_INFO "SETR_CLAVIER_IRQ : Initialisation du driver commencee\n");

    majorNumber = register_chrdev(0, DEV_NAME, &fops);
    if (majorNumber<0){
      printk(KERN_ALERT "SETR_CLAVIER_IRQ : Erreur lors de l'appel a register_chrdev!\n");
      return majorNumber;
    }

    // Création de la classe de périphérique
    setrClasse = class_create(CLS_NAME);
    if (IS_ERR(setrClasse)){
      unregister_chrdev(majorNumber, DEV_NAME);
      printk(KERN_ALERT "SETR_CLAVIER_IRQ : Erreur lors de la creation de la classe de peripherique\n");
      return PTR_ERR(setrClasse);
    }
    printk(KERN_INFO "SETR_CLAVIER_IRQ : device class OK\n");

    // Création du pilote de périphérique associé
    setrDevice = device_create(setrClasse, NULL, MKDEV(majorNumber, 0), NULL, DEV_NAME);
    if (IS_ERR(setrDevice)){
      class_destroy(setrClasse);
      unregister_chrdev(majorNumber, DEV_NAME);
      printk(KERN_ALERT "SETR_CLAVIER_IRQ : Erreur lors de la creation du pilote de peripherique\n");
      return PTR_ERR(setrDevice);
    }


   // TODO
    // Initialisez les GPIO. Pour ce faire, vous DEVEZ utiliser l'API "GPIO Descriptor Consumer Interface"
    // https://www.kernel.org/doc/html/v6.1/driver-api/gpio/consumer.html
    //
    // Plus précisément, dans l'initialisation, vous devrez:
    // 1) Utiliser gpiod_add_lookup_table pour enregistrer la table de correspondances
    //      définie dans gpios_table (plus haut)
    // 2) Appeler 2 fois gpiod_get_array (une fois pour les GPIO en lecture, une fois pour ceux en écriture)
    //      et mettre le résultat dans gpioLecture et gpioEcriture (déclarés plus haut). Validez que les
    //      appels de fonction réussissent en utilisant la macro IS_ERR().
    //      N'oubliez pas de leur donner la bonne direction!
    //
    // Finalement, vous devez enregistrer une IRQ pour chaque GPIO en entrée. Utilisez
    // pour ce faire gpiod_to_irq, ce qui vous donnera le numéro d'interruption lié à un
    // GPIO en particulier, puis appelez request_irq comme présenté plus bas pour
    // enregistrer la fonction de traitement de l'interruption.
    // Attention, cette fonction devra être appelée 4 fois (une fois pour chaque GPIO)!
    //
    // Vous devez également initialiser le mutex de synchronisation.

    ok = request_irq(irqno,                 // Le numéro de l'interruption, obtenue avec gpio_to_irq
         (irq_handler_t) setr_irq_handler,  // Pointeur vers la routine de traitement de l'interruption
         IRQF_TRIGGER_RISING,               // On veut une interruption sur le front montant (lorsque le bouton est pressé)
         "setr_irq_handler",                // Le nom de notre interruption
         NULL);                             // Paramètre supplémentaire inutile pour vous
    if(ok != 0){
        printk(KERN_ALERT "Erreur (%d) lors de l'enregistrement IRQ #{%d}!\n", ok, irqno);
        device_destroy(setrClasse, MKDEV(majorNumber, 0));
        class_destroy(setrClasse);
        unregister_chrdev(majorNumber, DEV_NAME);
        return ok;
    }


    printk(KERN_INFO "SETR_CLAVIER_IRQ : Fin de l'Initialisation!\n"); // Made it! device was initialized

    return 0;
}


static void __exit setrclavier_exit(void){
    // TODO
    // Déclarez _toutes_ vos variables locales ici (le module est compilé avec un standard générant
    // un warning si une variable est déclarée après toute ligne de code)

    // TODO
    // Écrivez le code permettant de relâcher (libérer) les GPIO
    // 1) Relâchez les interruptions qui ont été précédemment enregistrées. 
    //      Utilisez free_irq(irqno, NULL) pour chaque GPIO
    // 2) Libérez les GPIO obtenus dans l'initialisation
    // 3) Retirez la table de correspondances avec gpiod_remove_lookup_table


    // On retire correctement les différentes composantes du pilote
    device_destroy(setrClasse, MKDEV(majorNumber, 0));
    class_destroy(setrClasse);
    unregister_chrdev(majorNumber, DEV_NAME);
    printk(KERN_INFO "SETR_CLAVIER_IRQ : Terminaison du driver\n");
}




static int dev_open(struct inode *inodep, struct file *filep){
    printk(KERN_INFO "SETR_CLAVIER_IRQ : Ouverture!\n");
    // Rien à faire ici, si ce n'est retourner une valeur de succès
    return 0;
}
static int dev_release(struct inode *inodep, struct file *filep){
   printk(KERN_INFO "SETR_CLAVIER_IRQ : Fermeture!\n");
   // Rien à faire ici, si ce n'est retourner une valeur de succès
   return 0;
}

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset){
    // TODO
    // Déclarez _toutes_ vos variables locales ici (le module est compilé avec un standard générant
    // un warning si une variable est déclarée après toute ligne de code)

    // TODO
    // Implémentez cette fonction de lecture
    //
    // Notez que si le reste de votre code est cohérent, elle peut être _exactement_
    // la même que pour le driver par polling. Le reste des explications est simplement
    // un copier coller des explications de l'autre driver.

    // Celle-ci doit copier N caractères dans le buffer fourni en paramètre, N étant le minimum
    // entre le nombre d'octets disponibles dans le buffer et le nombre d'octets demandés (paramètre len).
    // Cette fonction DOIT se synchroniser au reste du module avec le mutex.
    // N'oubliez pas d'utiliser copy_to_user et NON memcpy pour copier les données dans le buffer
    // de l'utilisateur!
    // Finalement, rappelez-vous que nous utilisons un buffer circulaire, c'est à dire que les nouvelles
    // écritures se font sur des adresses croissantes, jusqu'à ce qu'on arrive à la fin du buffer et qu'on
    // revienne alors à 0. Il est donc tout à fait possible que posCouranteEcriture soit INFÉRIEUR à
    // posCouranteLecture, et vous devez gérer ce cas sans perdre de caractères et en respectant les
    // autres conditions (par exemple, ne jamais copier plus que len caractères).
}


// On enregistre les fonctions d'initialisation et de destruction
module_init(setrclavier_init);
module_exit(setrclavier_exit);

// Description du module
MODULE_LICENSE("GPL");            // Licence : laissez "GPL"
MODULE_AUTHOR("Vous!");           // Vos noms
MODULE_DESCRIPTION("Lecteur de clavier externe, avec interruptions");  // Description du module
MODULE_VERSION("2.0");            // Numéro de version
