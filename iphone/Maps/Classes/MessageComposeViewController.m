
#import "MessageComposeViewController.h"
#import "UIKitCategories.h"

@interface MessageComposeViewController ()

@end

@implementation MessageComposeViewController

- (void)viewDidLoad
{
  [super viewDidLoad];

  if (!SYSTEM_VERSION_IS_LESS_THAN(@"7"))
    self.navigationBar.tintColor = [UIColor whiteColor];
}

- (void)didReceiveMemoryWarning
{
  [super didReceiveMemoryWarning];
  // Dispose of any resources that can be recreated.
}

@end
